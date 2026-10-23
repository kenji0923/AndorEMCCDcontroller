/**
 * @file MainWindow.cc
 * @brief Implementation of AndorEMCCDcontroller::MainWindow.
 */

#include "MainWindow.h"

#include <algorithm>
#include <cmath>
#include <thread>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QUdpSocket>
#include <QPointer>
#include <QPushButton>
#include <QFileInfo>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>

#include <AndorEMCCDcontroller/ImageIO.h>

#include "AnalysisPanel.h"
#include "ImageView.h"
#include "SettingsPanel.h"

namespace AndorEMCCDcontroller {

namespace {

// Display refresh cap (~60 Hz). Analysis and UDP run on every frame anyway.
constexpr int kRenderIntervalMs = 16;
constexpr double kStatusTimeoutS = 3.0;

enum ScaleMode { kScaleMinMax = 0, kScalePercentile = 1, kScaleFixed = 2 };

QString TemperatureColor(const std::string& status) {
  if (status == "stabilized") return QStringLiteral("#2e7d32");
  if (status == "not_stabilized") return QStringLiteral("#b8860b");
  if (status == "off") return QStringLiteral("#606060");
  return QStringLiteral("#c62828");
}

QString Pretty(const std::string& s) {
  QString t = QString::fromStdString(s);
  t.replace('_', ' ');
  return t;
}

/** EM gain in effect for a settings object (mirrors EffectiveEmGain). */
int ExpectedEmGain(const Json& settings) {
  return settings.value("output_amplifier", 0) == 0 &&
                 settings.value("em_gain_enabled", false)
             ? settings.value("em_gain", 0)
             : 0;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle(tr("iXon EMCCD controller"));
  BuildUi();
  LoadPreferences();
  ReconfigureAnalysis();
  UpdateColormap();

  connect(&render_timer_, &QTimer::timeout, this, &MainWindow::RenderLatest);
  // The default coarse timer has ~15.6 ms steps on Windows, which beat
  // against the camera period and dropped the display to ~21 of 31 fps.
  render_timer_.setTimerType(Qt::PreciseTimer);
  render_timer_.start(kRenderIntervalMs);
  connect(&slow_timer_, &QTimer::timeout, this, [this] {
    trend_panel_->Refresh(processor_);
    UpdateStreamRecordingStatus();
    // Keep the control lease alive (the backend frees it after ~10 s).
    if (have_control_ && control_ && UnixTimeNow() - last_keepalive_ > 2.0) {
      last_keepalive_ = UnixTimeNow();
      Request("ping", Json::object(), nullptr, 2000);
    }
    if (control_) {
      const bool stale = UnixTimeNow() - last_status_time_ > kStatusTimeoutS;
      connection_label_->setText(
          (stale ? tr("No status from ") : tr("Connected: ")) +
          QString::fromStdString(control_->endpoint()));
      connection_label_->setStyleSheet(stale ? "color: #c62828;" : "");
    }
    const std::string udp_error = processor_.UdpError();
    analysis_panel_->SetUdpStatus(
        udp_error.empty()
            ? tr("Packets sent: %1").arg(processor_.UdpPacketsSent())
            : QString::fromStdString(udp_error));
  });
  slow_timer_.start(250);
  UpdateControl(nullptr);
}

MainWindow::~MainWindow() { Disconnect(); }

void MainWindow::BuildUi() {
  auto* central = new QWidget(this);
  auto* row = new QHBoxLayout(central);
  row->setContentsMargins(2, 2, 2, 2);
  view_ = new ImageView(central);
  colorbar_ = new ColorBar(central);
  row->addWidget(view_, 1);
  row->addWidget(colorbar_);
  setCentralWidget(central);

  // Toolbar.
  QToolBar* tb = addToolBar(tr("Acquisition"));
  tb->setObjectName("acquisition_toolbar");
  tb->addAction(tr("Connect…"), this, &MainWindow::ShowConnectDialog);
  control_action_ =
      tb->addAction(tr("Request control"), this, &MainWindow::ToggleControl);
  control_action_->setToolTip(
      tr("Only the client holding control can change settings or start and "
         "stop acquisitions"));
  tb->addSeparator();
  QAction* live = tb->addAction(tr("Live"), this, &MainWindow::StartLive);
  live->setToolTip(tr("Continuous acquisition with the current settings"));
  QAction* single = tb->addAction(tr("Single"), this, &MainWindow::AcquireSingle);
  arm_frames_ = new QSpinBox(this);
  arm_frames_->setRange(1, 1000000);
  arm_frames_->setValue(10);
  arm_frames_->setPrefix(tr("N = "));
  QAction* series = tb->addAction(tr("Series"), this, &MainWindow::ArmSeries);
  series->setToolTip(tr("Kinetic series of N frames (uses the trigger mode)"));
  tb->addWidget(arm_frames_);
  QAction* stop = tb->addAction(tr("Stop"), this, &MainWindow::StopAcquisition);
  QAction* trigger =
      tb->addAction(tr("Software trigger"), this, &MainWindow::SoftwareTrigger);
  em_gain_action_ = tb->addAction(tr("EM gain off"));
  em_gain_action_->setCheckable(true);
  em_gain_action_->setToolTip(
      tr("Switch the EM gain on or off; the value is set under Camera "
         "settings > EM gain"));
  connect(em_gain_action_, &QAction::triggered, this, [this](bool on) {
    ApplySettings({{"em_gain_enabled", on}});
    UpdateEmGain();  // Reverts the button if the request failed.
  });
  mutating_actions_ = {live, single, series, stop, trigger, em_gain_action_};
  tb->addSeparator();
  tb->addAction(tr("Save frame…"), this, &MainWindow::SaveCurrentFrame);
  tb->addAction(tr("Save view…"), this, &MainWindow::SaveView)
      ->setToolTip(tr("False-color PNG of the displayed image"));
  tb->addAction(tr("Record…"), this, &MainWindow::RecordFrames)
      ->setToolTip(tr("Save the next N live frames"));
  tb->addAction(tr("Save run…"), this, &MainWindow::SaveRunFromBackend)
      ->setToolTip(tr("Fetch a complete run from the backend history"));
  tb->addSeparator();
  draw_roi_action_ = tb->addAction(tr("Draw ROI"));
  draw_roi_action_->setCheckable(true);
  tb->addAction(tr("Fit"), view_, &ImageView::FitToWindow);
  tb->addAction(tr("1:1"), view_, &ImageView::ZoomOneToOne);

  // Settings dock.
  settings_panel_ = new SettingsPanel(this);
  auto* settings_dock = new QDockWidget(tr("Camera settings"), this);
  settings_dock->setObjectName("settings_dock");
  settings_dock->setWidget(settings_panel_);
  addDockWidget(Qt::RightDockWidgetArea, settings_dock);
  connect(settings_panel_, &SettingsPanel::ApplyRequested, this,
          &MainWindow::ApplySettings);
  connect(settings_panel_, &SettingsPanel::ReloadRequested, this,
          &MainWindow::ReloadSettings);

  // Analysis dock.
  analysis_panel_ = new AnalysisPanel(this);
  auto* analysis_dock = new QDockWidget(tr("Analysis"), this);
  analysis_dock->setObjectName("analysis_dock");
  analysis_dock->setWidget(analysis_panel_);
  addDockWidget(Qt::RightDockWidgetArea, analysis_dock);
  tabifyDockWidget(settings_dock, analysis_dock);
  settings_dock->raise();
  connect(analysis_panel_, &AnalysisPanel::ConfigChanged, this,
          &MainWindow::ReconfigureAnalysis);
  connect(analysis_panel_, &AnalysisPanel::UdpChanged, this,
          [this] { processor_.SetUdp(analysis_panel_->Udp()); });
  connect(analysis_panel_, &AnalysisPanel::DrawRoiToggled, draw_roi_action_,
          &QAction::setChecked);
  connect(draw_roi_action_, &QAction::toggled, view_, &ImageView::SetDrawRoiMode);

  // Trend dock.
  trend_panel_ = new TrendPanel(this);
  auto* trend_dock = new QDockWidget(tr("Trend"), this);
  trend_dock->setObjectName("trend_dock");
  trend_dock->setWidget(trend_panel_);
  addDockWidget(Qt::BottomDockWidgetArea, trend_dock);
  connect(trend_panel_, &TrendPanel::ClearRequested, this,
          [this] { processor_.ClearHistory(); });

  // Status dock.
  auto* status = new QWidget(this);
  auto* sform = new QFormLayout(status);
  status_state_ = new QLabel(tr("disconnected"), status);
  status_control_ = new QLabel(tr("nobody"), status);
  status_control_->setWordWrap(true);
  status_temp_ = new QLabel(status);
  QFont big = status_temp_->font();
  big.setPointSizeF(big.pointSizeF() * 1.4);
  big.setBold(true);
  status_temp_->setFont(big);
  status_run_ = new QLabel(status);
  status_rate_ = new QLabel(status);
  status_timing_ = new QLabel(status);
  status_timing_->setWordWrap(true);
  status_em_gain_ = new QLabel(status);
  status_error_ = new QLabel(status);
  status_error_->setWordWrap(true);
  status_error_->setStyleSheet("color: #c62828;");
  sform->addRow(tr("State"), status_state_);
  sform->addRow(tr("Control"), status_control_);
  sform->addRow(tr("Sensor"), status_temp_);
  sform->addRow(tr("EM gain"), status_em_gain_);
  sform->addRow(tr("Run"), status_run_);
  sform->addRow(tr("Rate"), status_rate_);
  sform->addRow(tr("Timings"), status_timing_);
  sform->addRow(status_error_);
  auto* status_dock = new QDockWidget(tr("Status"), this);
  status_dock->setObjectName("status_dock");
  status_dock->setWidget(status);
  addDockWidget(Qt::LeftDockWidgetArea, status_dock);

  BuildDisplayDock();
  BuildStreamRecordingDock();

  // Image interaction.
  connect(view_, &ImageView::CursorMoved, this, [this](int x, int y, bool ok) {
    if (!ok || !shown_) {
      cursor_label_->clear();
      return;
    }
    const FrameHeader& h = displayed_frame_.header;
    const double v = PixelValue(h, displayed_frame_.data->data(), x, y);
    // Sensor coordinates (1-based) of the pixel, for the settings fields.
    cursor_label_->setText(tr("x %1  y %2  (sensor %3, %4)  value %5")
                               .arg(x)
                               .arg(y)
                               .arg(h.x0 + x * h.hbin)
                               .arg(h.y0 + y * h.vbin)
                               .arg(v, 0, 'g', 6));
  });
  connect(view_, &ImageView::RoiDrawn, this, [this](QRect r) {
    analysis_panel_->AddRoi(r);
    draw_roi_action_->setChecked(false);
  });
  connect(view_, &ImageView::RoiMoved, this, [this](int i, int x, int y) {
    analysis_panel_->MoveRoi(i, x, y);
  });
  connect(view_, &ImageView::RoiSelected, analysis_panel_,
          &AnalysisPanel::SelectRoi);

  cursor_label_ = new QLabel(this);
  connection_label_ = new QLabel(tr("Not connected"), this);
  statusBar()->addWidget(cursor_label_, 1);
  statusBar()->addPermanentWidget(connection_label_);
}

void MainWindow::BuildDisplayDock() {
  auto* w = new QWidget(this);
  auto* form = new QFormLayout(w);
  colormap_ = new QComboBox(w);
  for (const auto& name : ColormapNames()) {
    colormap_->addItem(QString::fromStdString(name));
  }
  colormap_->setCurrentText(QStringLiteral("viridis"));
  reverse_cmap_ = new QCheckBox(tr("Reverse"), w);
  scale_mode_ = new QComboBox(w);
  scale_mode_->addItems({tr("Auto (min to max)"), tr("Auto (percentiles)"),
                         tr("Fixed")});
  scale_lo_ = new QDoubleSpinBox(w);
  scale_hi_ = new QDoubleSpinBox(w);
  for (auto* s : {scale_lo_, scale_hi_}) {
    s->setRange(-1e12, 1e12);
    s->setDecimals(1);
    s->setKeyboardTracking(false);
  }
  scale_hi_->setValue(1000.0);
  pct_lo_ = new QDoubleSpinBox(w);
  pct_hi_ = new QDoubleSpinBox(w);
  for (auto* s : {pct_lo_, pct_hi_}) {
    s->setRange(0.0, 100.0);
    s->setDecimals(2);
    s->setSuffix(QStringLiteral(" %"));
  }
  pct_lo_->setValue(0.5);
  pct_hi_->setValue(99.5);
  log_scale_ = new QCheckBox(tr("Logarithmic"), w);
  flip_h_ = new QCheckBox(tr("Flip horizontally"), w);
  flip_v_ = new QCheckBox(tr("Flip vertically"), w);
  rotation_ = new QComboBox(w);
  rotation_->addItems({tr("0°"), tr("90° clockwise"), tr("180°"),
                       tr("270° clockwise")});
  rotation_->setToolTip(tr("Display only, applied after the flips. Saved raw "
                           "data are not rotated; the orientation is recorded "
                           "in the file (TIFF Orientation tag, FITS keywords, "
                           "JSON metadata)."));

  auto* cmap_row = new QHBoxLayout();
  cmap_row->addWidget(colormap_, 1);
  cmap_row->addWidget(reverse_cmap_);
  average_frames_ = new QSpinBox(w);
  average_frames_->setObjectName("display_average_frames");
  average_frames_->setRange(1, 63);
  average_frames_->setKeyboardTracking(false);
  average_frames_->setToolTip(tr("Average the latest received images for display only. "
                                 "1 disables averaging; count must be below the client buffer length."));
  average_used_ = new QLabel(w);
  form->addRow(tr("Average (frames)"), average_frames_);
  form->addRow(tr("Images averaged"), average_used_);
  form->addRow(tr("Colormap"), cmap_row);
  form->addRow(tr("Scale"), scale_mode_);
  form->addRow(tr("Minimum"), scale_lo_);
  form->addRow(tr("Maximum"), scale_hi_);
  form->addRow(tr("Lower percentile"), pct_lo_);
  form->addRow(tr("Upper percentile"), pct_hi_);
  form->addRow(log_scale_);
  form->addRow(flip_h_);
  form->addRow(flip_v_);
  form->addRow(tr("Rotation"), rotation_);

  auto* dock = new QDockWidget(tr("Display"), this);
  dock->setObjectName("display_dock");
  dock->setWidget(w);
  addDockWidget(Qt::LeftDockWidgetArea, dock);

  auto redraw = [this] {
    last_rendered_seq_ = UINT64_MAX;
    const bool fixed = scale_mode_->currentIndex() == kScaleFixed;
    scale_lo_->setEnabled(fixed);
    scale_hi_->setEnabled(fixed);
    pct_lo_->setEnabled(scale_mode_->currentIndex() == kScalePercentile);
    pct_hi_->setEnabled(scale_mode_->currentIndex() == kScalePercentile);
    RenderLatest();
  };
  connect(average_frames_, &QSpinBox::valueChanged, this, redraw);
  connect(colormap_, &QComboBox::currentTextChanged, this,
          &MainWindow::UpdateColormap);
  connect(reverse_cmap_, &QCheckBox::toggled, this, &MainWindow::UpdateColormap);
  connect(scale_mode_, &QComboBox::currentIndexChanged, this, redraw);
  for (auto* s : {scale_lo_, scale_hi_, pct_lo_, pct_hi_}) {
    connect(s, &QDoubleSpinBox::valueChanged, this, redraw);
  }
  connect(log_scale_, &QCheckBox::toggled, this, redraw);
  auto flip = [this] {
    view_->SetFlip(flip_h_->isChecked(), flip_v_->isChecked());
  };
  connect(flip_h_, &QCheckBox::toggled, this, flip);
  connect(flip_v_, &QCheckBox::toggled, this, flip);
  connect(rotation_, &QComboBox::currentIndexChanged, this,
          [this](int index) { view_->SetRotation(90 * index); });
  redraw();
}

void MainWindow::BuildStreamRecordingDock() {
  auto* widget = new QWidget(this);
  auto* form = new QFormLayout(widget);
  stream_path_ = new QLineEdit(widget);
  stream_path_->setObjectName("stream_recording_path");
  stream_path_->setPlaceholderText(tr("Select a new .tif file"));
  stream_browse_ = new QPushButton(tr("Browse…"), widget);
  connect(stream_browse_, &QPushButton::clicked, this, [this] {
    QString path = QFileDialog::getSaveFileName(
        this, tr("Stream to BigTIFF"),
        stream_path_->text().isEmpty()
            ? QStringLiteral("stream_%1.tif").arg(
                  QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz"))
            : stream_path_->text(),
        tr("BigTIFF (*.tif *.tiff)"));
    if (!path.isEmpty()) {
      if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".tif");
      stream_path_->setText(path);
    }
  });
  auto* path_row = new QHBoxLayout;
  path_row->addWidget(stream_path_, 1);
  path_row->addWidget(stream_browse_);
  form->addRow(tr("File (BigTIFF)"), path_row);
  stream_fps_ = new QDoubleSpinBox(widget);
  stream_fps_->setObjectName("stream_recording_fps");
  stream_fps_->setRange(0, 100000);
  stream_fps_->setDecimals(3);
  stream_fps_->setValue(10);
  stream_fps_->setSpecialValueText(tr("All received frames"));
  stream_fps_->setKeyboardTracking(false);
  stream_fps_->setToolTip(tr("0: no rate limit. Sampling uses frame acquisition timestamps. "
                            "Raw frames are saved independently of display averaging."));
  form->addRow(tr("Maximum save rate (fps)"), stream_fps_);
  stream_toggle_ = new QPushButton(tr("Start saving"), widget);
  stream_toggle_->setObjectName("stream_recording_toggle");
  connect(stream_toggle_, &QPushButton::clicked, this,
          &MainWindow::ToggleStreamRecording);
  form->addRow(stream_toggle_);
  stream_status_ = new QLabel(tr("Not saving"), widget);
  stream_status_->setWordWrap(true);
  stream_status_->setObjectName("stream_recording_status");
  form->addRow(stream_status_);
  auto* dock = new QDockWidget(tr("Stream recording"), this);
  dock->setObjectName("stream_recording_dock");
  dock->setWidget(widget);
  addDockWidget(Qt::RightDockWidgetArea, dock);
  if (auto* settings = findChild<QDockWidget*>("settings_dock")) {
    tabifyDockWidget(settings, dock);
  }
  dock->raise();
}

void MainWindow::StartStreamRecording(const QString& path, double max_fps) {
  stream_path_->setText(path);
  stream_fps_->setValue(max_fps);
  ToggleStreamRecording();
}

void MainWindow::ToggleStreamRecording() {
  if (stream_recorder_.Status().active) {
    stream_recorder_.Stop();
    UpdateStreamRecordingStatus();
    return;
  }
  if (!subscriber_) {
    QMessageBox::warning(this, tr("Stream recording"), tr("Connect to a backend first."));
    return;
  }
  if (stream_path_->text().isEmpty()) stream_browse_->click();
  if (stream_path_->text().isEmpty()) return;
  std::string error;
  if (!stream_recorder_.Start(stream_path_->text().toStdString(),
                              stream_fps_->value(), SaveMetadata(),
                              static_cast<std::size_t>(address_.buffer_frames),
                              std::size_t{256} << 20, &error)) {
    QMessageBox::warning(this, tr("Stream recording"), QString::fromStdString(error));
  }
  UpdateStreamRecordingStatus();
}

void MainWindow::UpdateStreamRecordingStatus() {
  const auto status = stream_recorder_.Status();
  stream_toggle_->setText(status.active ? tr("Stop saving") : tr("Start saving"));
  stream_path_->setEnabled(!status.active);
  stream_browse_->setEnabled(!status.active);
  stream_fps_->setEnabled(!status.active);
  stream_status_->setText(
      tr("%1 — saved %2, rate skipped %3, queue dropped %4, queued %5, failed %6")
          .arg(status.active ? tr("Saving") : tr("Stopped"))
          .arg(status.saved).arg(status.rate_skipped).arg(status.queue_dropped)
          .arg(static_cast<qulonglong>(status.queued)).arg(status.failed) +
      (status.error.empty() ? QString() : QStringLiteral("\n") +
                                         QString::fromStdString(status.error)));
  stream_status_->setStyleSheet(status.error.empty() && !status.queue_dropped
                                   ? "" : "color: #c62828;");
}

void MainWindow::UpdateColormap() {
  cmap_ = GetColormap(colormap_->currentText().toStdString(),
                      reverse_cmap_->isChecked());
  colorbar_->SetColormap(std::vector<unsigned int>(cmap_.begin(), cmap_.end()));
  last_rendered_seq_ = UINT64_MAX;
  RenderLatest();
}

void MainWindow::ShowConnectDialog() {
  QDialog dialog(this);
  dialog.setWindowTitle(tr("Connect to backend"));
  auto* form = new QFormLayout(&dialog);
  auto* name = new QLineEdit(QString::fromStdString(address_.client_name),
                             &dialog);
  name->setPlaceholderText(QStringLiteral("client"));
  name->setToolTip(tr("Other clients see you as name@your-ip"));
  auto* host = new QLineEdit(QString::fromStdString(address_.host), &dialog);
  auto* control = new QSpinBox(&dialog);
  auto* data_port = new QSpinBox(&dialog);
  control->setRange(1, 65535);
  data_port->setRange(1, 65535);
  control->setValue(address_.control_port);
  data_port->setValue(address_.data_port);
  auto* transport = new QComboBox(&dialog);
  transport->addItems({tr("Automatic (IPC on the backend host)"),
                       tr("TCP only"), tr("IPC only")});
  transport->setCurrentIndex(address_.use_ipc < 0    ? 0
                             : address_.use_ipc == 0 ? 1
                                                     : 2);
  auto* ipc_dir = new QLineEdit(QString::fromStdString(address_.ipc_dir),
                                &dialog);
  auto* buffer = new QSpinBox(&dialog);
  buffer->setRange(1, 1000000);
  buffer->setValue(address_.buffer_frames);
  form->addRow(tr("Client buffer (frames)"), buffer);
  form->addRow(tr("Your name"), name);
  form->addRow(tr("Host"), host);
  form->addRow(tr("Control port"), control);
  form->addRow(tr("Data port"), data_port);
  form->addRow(tr("Transport"), transport);
  form->addRow(tr("IPC directory"), ipc_dir);
  auto* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  form->addRow(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted) return;
  ServerAddress a;
  a.buffer_frames = buffer->value();
  a.client_name = name->text().trimmed().toStdString();
  a.host = host->text().trimmed().toStdString();
  a.control_port = control->value();
  a.data_port = data_port->value();
  a.use_ipc = transport->currentIndex() == 0 ? -1
              : transport->currentIndex() == 1 ? 0
                                               : 1;
  a.ipc_dir = ipc_dir->text().trimmed().toStdString();
  ConnectTo(a);
}

void MainWindow::Disconnect() {
  if (control_ && have_control_) {
    try {
      control_->Request("release_control", Json::object(), 1000);
    } catch (const std::exception&) {
    }
  }
  have_control_ = false;
  if (subscriber_) subscriber_->Stop();
  subscriber_.reset();
  stream_recorder_.Stop();
  UpdateStreamRecordingStatus();
  control_.reset();
}

void MainWindow::ConnectTo(const ServerAddress& address) {
  average_frames_->setMaximum(std::max(1, address.buffer_frames - 1));
  average_frames_->setEnabled(address.buffer_frames > 1);
  Disconnect();
  address_ = address;
  std::string control_ep;
  std::string data_ep;
  ResolveEndpoints(address, &control_ep, &data_ep);
  control_ = std::make_unique<ControlClient>(&context_, control_ep);
  control_->SetIdentity(address.client_name, LocalAddress());
  connection_label_->setText(tr("Connecting to %1…")
                                 .arg(QString::fromStdString(control_ep)));
  Json pong;
  if (!Request("ping", Json::object(), &pong, 2000)) {
    QMessageBox::warning(this, tr("Connection failed"),
                         tr("No answer from the backend at %1.\nCheck that "
                            "the service runs and the ports are open.")
                             .arg(QString::fromStdString(control_ep)));
    control_.reset();
    connection_label_->setText(tr("Not connected"));
    return;
  }
  QPointer<MainWindow> self(this);
  subscriber_ = std::make_unique<FrameSubscriber>(
      &context_, data_ep,
      [this](Frame frame) {
        stream_recorder_.Submit(frame);
        processor_.Process(std::move(frame));
      },
      [self](const Json& status) {
        QMetaObject::invokeMethod(
            qApp,
            [self, status] {
              if (self) self->OnStatus(status);
            },
            Qt::QueuedConnection);
      }, address.buffer_frames, control_ep);
  subscriber_->Start();
  last_status_time_ = UnixTimeNow();
  ReloadSettings();
  Json control;
  if (Request("get_control", Json::object(), &control)) UpdateControl(control);
  statusBar()->showMessage(
      tr("Connected (%1)").arg(pong.value("simulated", false)
                                   ? tr("simulated camera")
                                   : tr("camera")),
      5000);
}

bool MainWindow::Request(const std::string& cmd, const Json& args,
                         Json* result, int timeout_ms) {
  if (!control_) {
    statusBar()->showMessage(tr("Not connected"), 5000);
    return false;
  }
  try {
    const Json r = control_->Request(cmd, args, timeout_ms);
    if (result) *result = r;
    return true;
  } catch (const std::exception& e) {
    statusBar()->showMessage(
        tr("%1: %2").arg(QString::fromStdString(cmd), e.what()), 10000);
    status_error_->setText(QString::fromUtf8(e.what()));
    return false;
  }
}

void MainWindow::ReloadSettings() {
  Json info;
  Json settings;
  if (Request("get_info", Json::object(), &info)) {
    info_ = info;
    settings_panel_->SetInfo(info);
    setWindowTitle(tr("iXon EMCCD controller — %1 (serial %2)")
                       .arg(QString::fromStdString(info.value("model", "")),
                            QString::fromStdString(info.value("serial", ""))));
  }
  if (Request("get_settings", Json::object(), &settings)) {
    settings_ = settings;
    settings_panel_->SetSettings(settings);
  }
  UpdateEmGain();
}

void MainWindow::ApplySettings(const Json& patch) {
  if (patch.empty()) {
    statusBar()->showMessage(tr("Nothing changed"), 3000);
    return;
  }
  Json result;
  if (!Request("set_settings", {{"settings", patch}}, &result)) return;
  settings_ = result["settings"];
  // EM gain range etc. depend on the applied settings.
  Json info;
  if (Request("get_info", Json::object(), &info)) {
    info_ = info;
    settings_panel_->SetInfo(info);
  }
  settings_panel_->SetSettings(settings_);
  UpdateEmGain();
  QStringList warnings;
  for (const auto& w : result.value("warnings", Json::array())) {
    warnings << QString::fromStdString(w.get<std::string>());
  }
  status_error_->setText(warnings.join('\n'));
  statusBar()->showMessage(warnings.isEmpty() ? tr("Settings applied")
                                              : tr("Applied with warnings"),
                           5000);
}

void MainWindow::StartLive() {
  if (Request("start", {{"settings", {{"acquisition_mode", "continuous"}}}},
              nullptr)) {
    ReloadSettings();
  }
}

void MainWindow::AcquireSingle() {
  if (Request("start", {{"settings", {{"acquisition_mode", "single"}}}},
              nullptr)) {
    ReloadSettings();
  }
}

void MainWindow::ArmSeries() {
  if (Request("arm", {{"n_frames", arm_frames_->value()}}, nullptr)) {
    ReloadSettings();
  }
}

void MainWindow::StopAcquisition() { Request("stop", Json::object(), nullptr); }

void MainWindow::SoftwareTrigger() {
  Request("software_trigger", Json::object(), nullptr);
}

std::string MainWindow::LocalAddress() const {
  // The source address of the route to the backend; for a local backend
  // (IPC) the first non-loopback IPv4 address of this machine.
  QUdpSocket probe;
  const QString host = QString::fromStdString(address_.host);
  probe.connectToHost(host == "localhost" ? QStringLiteral("127.0.0.1") : host,
                      static_cast<quint16>(address_.control_port));
  if (probe.waitForConnected(200)) {
    const QHostAddress local = probe.localAddress();
    if (!local.isLoopback() && local.protocol() == QAbstractSocket::IPv4Protocol) {
      return local.toString().toStdString();
    }
  }
  for (const QHostAddress& a : QNetworkInterface::allAddresses()) {
    if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback()) {
      return a.toString().toStdString();
    }
  }
  return "127.0.0.1";
}

void MainWindow::ToggleControl() {
  if (!control_) return;
  if (have_control_) {
    Json control;
    if (Request("release_control", Json::object(), &control)) {
      UpdateControl(control);
    }
    return;
  }
  try {
    UpdateControl(control_->Request("acquire_control", Json::object()));
  } catch (const ClientError& e) {
    if (e.code() != "control_denied") {
      statusBar()->showMessage(QString::fromUtf8(e.what()), 8000);
      return;
    }
    const QString holder =
        QString::fromStdString(e.result().value("holder", std::string("?")));
    const auto answer = QMessageBox::question(
        this, tr("Control is taken"),
        tr("%1 holds control of the camera.\n\nTake over anyway? Their "
           "changes will be rejected from now on.")
            .arg(holder),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;
    Json control;
    if (Request("acquire_control", {{"force", true}}, &control)) {
      UpdateControl(control);
    }
  }
}

void MainWindow::UpdateControl(const Json& control) {
  const bool held = control.is_object();
  const bool mine = held && control_ &&
                    control.value("holder_id", std::string()) ==
                        control_->client_id();
  if (have_control_ && !mine && held) {
    statusBar()->showMessage(
        tr("Control was taken by %1")
            .arg(QString::fromStdString(control.value("holder", ""))),
        10000);
  }
  have_control_ = mine;
  const QString holder =
      held ? QString::fromStdString(control.value("holder", std::string()))
           : tr("nobody");
  status_control_->setText(mine ? tr("%1 (this client)").arg(holder) : holder);
  status_control_->setStyleSheet(mine ? "color: #2e7d32; font-weight: bold;"
                                      : "");
  control_action_->setText(mine ? tr("Release control") : tr("Request control"));
  control_action_->setEnabled(control_ != nullptr);
  const QString reason =
      held ? tr("Control is held by %1").arg(holder)
           : tr("Request control to change settings");
  for (QAction* a : mutating_actions_) {
    a->setEnabled(mine);
    a->setStatusTip(mine ? QString() : reason);
  }
  settings_panel_->SetEditable(mine, reason);
}

void MainWindow::UpdateEmGain() {
  const bool on = settings_.value("em_gain_enabled", false);
  const bool em_amp = settings_.value("output_amplifier", 0) == 0;
  const int gain = status_.value("em_gain", ExpectedEmGain(settings_));
  QString text;
  if (!em_amp) {
    text = tr("conventional amplifier");
  } else if (!on) {
    text = tr("off (set point %1)").arg(settings_.value("em_gain", 0));
  } else {
    text = tr("on, %1").arg(gain);
  }
  status_em_gain_->setText(text);
  status_em_gain_->setStyleSheet(em_amp && on ? "color: #e65100;" : "");
  em_gain_action_->setChecked(em_amp && on);
  em_gain_action_->setText(em_amp && on ? tr("EM gain %1").arg(gain)
                                        : tr("EM gain off"));
}

void MainWindow::OnStatus(const Json& status) {
  if (status.contains("stream_error")) {
    status_error_->setText(QString::fromStdString(status.at("stream_error").get<std::string>()));
    return;
  }
  const std::string previous = status_.value("state", std::string());
  status_ = status;
  last_status_time_ = UnixTimeNow();
  const std::string state = status.value("state", std::string("unknown"));
  status_state_->setText(Pretty(state));
  UpdateControl(status.value("control", Json()));
  if (previous == "disconnected" && state == "idle") {
    ReloadSettings();
  } else if (state != "disconnected" && status.contains("em_gain") &&
             settings_.contains("em_gain") &&
             status["em_gain"] != ExpectedEmGain(settings_)) {
    ReloadSettings();  // Another client changed the gain.
  }
  UpdateEmGain();

  const std::string ts = status.value("temperature_status", std::string());
  status_temp_->setText(tr("%1 °C").arg(status.value("temperature_c", 0.0), 0,
                                        'f', 1));
  status_temp_->setStyleSheet("color: " + TemperatureColor(ts) + ";");
  status_temp_->setToolTip(tr("Cooler %1, %2")
                               .arg(status.value("cooler_on", false)
                                        ? tr("on")
                                        : tr("off"),
                                    Pretty(ts)));
  const auto expected = status.value("frames_expected", std::int64_t{0});
  status_run_->setText(
      tr("#%1 %2: %3%4 frames")
          .arg(status.value("run_id", std::uint64_t{0}))
          .arg(QString::fromStdString(status.value("run_mode", std::string())))
          .arg(status.value("frames_in_run", std::int64_t{0}))
          .arg(expected > 0 ? tr(" / %1").arg(expected) : QString()));
  status_rate_->setText(
      tr("camera %1 fps, display %2 fps, dropped %3")
          .arg(status.value("fps", 0.0), 0, 'f', 1)
          .arg(display_fps_, 0, 'f', 1)
          .arg(subscriber_ ? subscriber_->Dropped() : 0));
  const Json t = status.value("timings", Json::object());
  status_timing_->setText(
      tr("exposure %1 ms, cycle %2 ms, readout %3 ms")
          .arg(t.value("exposure_s", 0.0) * 1e3, 0, 'g', 4)
          .arg(t.value("kinetic_s", 0.0) * 1e3, 0, 'g', 4)
          .arg(t.value("readout_s", 0.0) * 1e3, 0, 'g', 4));
  const std::string error = status.value("last_error", std::string());
  if (!error.empty()) status_error_->setText(QString::fromStdString(error));
}

void MainWindow::RenderLatest() {
  auto latest = processor_.Latest();
  if (!latest || !latest->frame.data) return;
  if (latest->frame.header.seq == last_rendered_seq_ && shown_ == latest) return;
  std::size_t used = 1;
  Frame display = subscriber_
      ? subscriber_->DisplayFrame(average_frames_->value(), &used,
                                  latest->frame.header.seq)
      : latest->frame;
  if (!display.data) return;
  displayed_frame_ = std::move(display);
  const FrameHeader& h = displayed_frame_.header;
  last_rendered_seq_ = latest->frame.header.seq;
  shown_ = latest;
  average_used_->setText(tr("%1 / %2").arg(static_cast<qulonglong>(used))
                                        .arg(average_frames_->value()));

  const std::uint8_t* pixels = displayed_frame_.data->data();
  double lo = scale_lo_->value();
  double hi = scale_hi_->value();
  if (scale_mode_->currentIndex() == kScaleMinMax) {
    FrameMinMax(h, pixels, &lo, &hi);
  } else if (scale_mode_->currentIndex() == kScalePercentile) {
    FramePercentiles(h, pixels, pct_lo_->value(), pct_hi_->value(), &lo, &hi);
  }
  if (scale_mode_->currentIndex() != kScaleFixed) {
    // Mirror the automatic range so that switching to fixed keeps it.
    const QSignalBlocker block_lo(scale_lo_);
    const QSignalBlocker block_hi(scale_hi_);
    scale_lo_->setValue(lo);
    scale_hi_->setValue(hi);
  }
  std::vector<std::uint8_t> idx(static_cast<std::size_t>(h.width) * h.height);
  MapToIndices(h, pixels, lo, hi, log_scale_->isChecked(), idx.data());
  QImage image(h.width, h.height, QImage::Format_RGB32);
  for (int y = 0; y < h.height; ++y) {
    auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
    const std::uint8_t* src = idx.data() + static_cast<std::size_t>(y) * h.width;
    for (int x = 0; x < h.width; ++x) line[x] = 0xff000000u | cmap_[src[x]];
  }
  view_->SetImage(image);
  view_->SetRois(analysis_panel_->Rois(), analysis_panel_->SelectedRoi());
  colorbar_->SetRange(lo, hi, log_scale_->isChecked());
  analysis_panel_->ShowValues(processor_.Names(), latest->values);
  if (!latest->frame.delivery.empty()) {
    status_rate_->setToolTip(QString::fromStdString(latest->frame.delivery.dump(2)));
  }

  ++display_frames_;
  const double now = UnixTimeNow();
  if (now - display_fps_t0_ >= 1.0) {
    display_fps_ = display_frames_ / (now - display_fps_t0_);
    display_frames_ = 0;
    display_fps_t0_ = now;
  }
}

void MainWindow::ReconfigureAnalysis() {
  const auto rois = analysis_panel_->Rois();
  const auto errors = processor_.Configure(rois, analysis_panel_->Formulas());
  analysis_panel_->SetFormulaErrors(errors);
  trend_panel_->SetNames(processor_.Names());
  analysis_panel_->SetVariableNames(processor_.Names());
  view_->SetRois(rois, analysis_panel_->SelectedRoi());
}

DisplayOrientation MainWindow::Orientation() const {
  DisplayOrientation o;
  o.rotation_cw_deg = 90 * rotation_->currentIndex();
  o.flip_horizontal = flip_h_->isChecked();
  o.flip_vertical = flip_v_->isChecked();
  return o;
}

Json MainWindow::SaveMetadata() const {
  return Json{{"settings", settings_},
              {"display", OrientationToJson(Orientation())},
              {"camera",
               {{"model", info_.value("model", "")},
                {"serial", info_.value("serial", "")},
                {"head_model", info_.value("head_model", "")}}},
              {"analysis", analysis_panel_->ToJson()},
              {"saved_at", QDateTime::currentDateTimeUtc()
                               .toString(Qt::ISODateWithMs)
                               .toStdString()}};
}

void MainWindow::SaveFramesTo(std::vector<Frame> frames, const QString& path) {
  ImageFormat format;
  if (!FormatFromPath(path.toStdString(), &format)) {
    QMessageBox::warning(this, tr("Save"),
                         tr("Unknown file type: %1\nUse .tif, .fits, .png or "
                            ".npy").arg(path));
    return;
  }
  const Json metadata = SaveMetadata();
  QPointer<MainWindow> self(this);
  statusBar()->showMessage(tr("Saving %1 frame(s)…").arg(frames.size()));
  // Large stacks are written on a worker thread to keep the display live.
  std::thread([self, frames = std::move(frames), path, format, metadata] {
    std::string error;
    const bool ok =
        WriteFrames(path.toStdString(), format, frames, metadata, &error);
    QMetaObject::invokeMethod(
        qApp,
        [self, ok, error, path, n = frames.size()] {
          if (!self) return;
          if (ok) {
            self->statusBar()->showMessage(
                self->tr("Saved %1 frame(s) to %2").arg(n).arg(path), 8000);
          } else {
            QMessageBox::warning(self, self->tr("Save failed"),
                                 QString::fromStdString(error));
          }
        },
        Qt::QueuedConnection);
  }).detach();
}

void MainWindow::SaveCurrentFrame() {
  if (!shown_) {
    statusBar()->showMessage(tr("No frame to save"), 3000);
    return;
  }
  const Frame frame = shown_->frame;
  QStringList filters;
  for (const auto& f : FormatFilters()) filters << QString::fromStdString(f);
  const QString path = QFileDialog::getSaveFileName(
      this, tr("Save frame"),
      QStringLiteral("frame_%1.tif")
          .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
      filters.join(";;"));
  if (!path.isEmpty()) SaveFramesTo({frame}, path);
}

void MainWindow::SaveView() {
  const QImage& image = view_->Image();
  if (image.isNull()) return;
  const QString path = QFileDialog::getSaveFileName(
      this, tr("Save false-color view"), QStringLiteral("view.png"),
      tr("PNG (*.png)"));
  if (path.isEmpty()) return;
  // As shown: flips, then rotation (the translation part is ignored here).
  const QImage flipped =
      image.transformed(view_->Orientation()).convertToFormat(QImage::Format_RGB32);
  std::vector<std::uint32_t> rgb(static_cast<std::size_t>(flipped.width()) *
                                 flipped.height());
  for (int y = 0; y < flipped.height(); ++y) {
    const auto* line = reinterpret_cast<const QRgb*>(flipped.constScanLine(y));
    for (int x = 0; x < flipped.width(); ++x) {
      rgb[static_cast<std::size_t>(y) * flipped.width() + x] = line[x] & 0xffffff;
    }
  }
  std::string error;
  if (!WritePngRgb(path.toStdString(), flipped.width(), flipped.height(),
                   rgb.data(), &error)) {
    QMessageBox::warning(this, tr("Save failed"), QString::fromStdString(error));
  }
}

void MainWindow::RecordFrames() {
  if (recording_) {
    processor_.CancelRecording();
    recording_ = false;
    statusBar()->showMessage(tr("Recording cancelled"), 3000);
    return;
  }
  bool ok = false;
  const int n = QInputDialog::getInt(this, tr("Record"),
                                     tr("Number of live frames to record:"),
                                     100, 1, 100000, 1, &ok);
  if (!ok) return;
  QStringList filters;
  for (const auto& f : FormatFilters()) filters << QString::fromStdString(f);
  const QString path = QFileDialog::getSaveFileName(
      this, tr("Record to"),
      QStringLiteral("record_%1.tif")
          .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
      filters.join(";;"));
  if (path.isEmpty()) return;
  recording_ = true;
  QPointer<MainWindow> self(this);
  processor_.StartRecording(
      static_cast<std::size_t>(n), [self, path](std::vector<Frame> frames) {
        // Runs on the receiver thread: hand over to the GUI thread.
        QMetaObject::invokeMethod(
            qApp,
            [self, path, frames = std::move(frames)]() mutable {
              if (!self) return;
              self->recording_ = false;
              self->SaveFramesTo(std::move(frames), path);
            },
            Qt::QueuedConnection);
      });
  statusBar()->showMessage(tr("Recording %1 frames… (press Record again to "
                              "cancel)").arg(n));
}

void MainWindow::SaveRunFromBackend() {
  if (!control_) return;
  Json runs;
  if (!Request("get_runs", Json::object(), &runs) || runs.empty()) {
    statusBar()->showMessage(tr("No runs in the backend history"), 5000);
    return;
  }
  QStringList items;
  for (auto it = runs.rbegin(); it != runs.rend(); ++it) {
    items << tr("Run %1: %2, %3 frame(s)%4")
                 .arg((*it)["run_id"].get<std::uint64_t>())
                 .arg(QString::fromStdString((*it)["mode"].get<std::string>()))
                 .arg((*it)["frames"].get<std::int64_t>())
                 .arg((*it)["complete"].get<bool>() ? QString()
                                                    : tr(" (running)"));
  }
  bool ok = false;
  const QString choice = QInputDialog::getItem(
      this, tr("Save run"), tr("Run:"), items, 0, false, &ok);
  if (!ok) return;
  const std::uint64_t run_id =
      runs[runs.size() - 1 - items.indexOf(choice)]["run_id"].get<std::uint64_t>();
  QStringList filters;
  for (const auto& f : FormatFilters()) filters << QString::fromStdString(f);
  const QString path = QFileDialog::getSaveFileName(
      this, tr("Save run"), QStringLiteral("run_%1.tif").arg(run_id),
      filters.join(";;"));
  if (path.isEmpty()) return;

  // Fetch on a separate connection so the GUI's requests are not blocked.
  const std::string endpoint = control_->endpoint();
  QPointer<MainWindow> self(this);
  statusBar()->showMessage(tr("Fetching run %1…").arg(run_id));
  std::thread([self, endpoint, run_id, path, ctx = &context_] {
    std::vector<Frame> frames;
    std::string error;
    try {
      ControlClient client(ctx, endpoint);
      client.Request("fetch", {{"run_id", run_id}}, 60000, &frames);
    } catch (const std::exception& e) {
      error = e.what();
    }
    QMetaObject::invokeMethod(
        qApp,
        [self, frames = std::move(frames), error, path]() mutable {
          if (!self) return;
          if (!error.empty()) {
            QMessageBox::warning(self, self->tr("Fetch failed"),
                                 QString::fromStdString(error));
            return;
          }
          self->SaveFramesTo(std::move(frames), path);
        },
        Qt::QueuedConnection);
  }).detach();
}

void MainWindow::LoadPreferences() {
  QSettings s;
  address_.buffer_frames = std::max(1, s.value("connection/buffer_frames", 64).toInt());
  address_.host = s.value("connection/host", "localhost").toString().toStdString();
  address_.client_name =
      s.value("connection/client_name", "").toString().toStdString();
  address_.control_port =
      s.value("connection/control_port", kDefaultControlPort).toInt();
  address_.data_port = s.value("connection/data_port", kDefaultDataPort).toInt();
  address_.use_ipc = s.value("connection/use_ipc", -1).toInt();
  address_.ipc_dir =
      s.value("connection/ipc_dir", kDefaultIpcDir).toString().toStdString();
  stream_fps_->setValue(s.value("stream_recording/max_fps", 10.0).toDouble());
  stream_path_->setText(s.value("stream_recording/path").toString());
  average_frames_->setMaximum(std::max(1, address_.buffer_frames - 1));
  average_frames_->setValue(s.value("display/average_frames", 1).toInt());
  average_frames_->setEnabled(address_.buffer_frames > 1);
  colormap_->setCurrentText(s.value("display/colormap", "viridis").toString());
  reverse_cmap_->setChecked(s.value("display/reverse", false).toBool());
  scale_mode_->setCurrentIndex(s.value("display/scale_mode", 1).toInt());
  scale_lo_->setValue(s.value("display/lo", 0.0).toDouble());
  scale_hi_->setValue(s.value("display/hi", 1000.0).toDouble());
  pct_lo_->setValue(s.value("display/pct_lo", 0.5).toDouble());
  pct_hi_->setValue(s.value("display/pct_hi", 99.5).toDouble());
  log_scale_->setChecked(s.value("display/log", false).toBool());
  flip_h_->setChecked(s.value("display/flip_h", false).toBool());
  flip_v_->setChecked(s.value("display/flip_v", false).toBool());
  rotation_->setCurrentIndex(std::clamp(s.value("display/rotation", 0).toInt(), 0, 3));
  arm_frames_->setValue(s.value("acquisition/series_frames", 10).toInt());
  const QString analysis = s.value("analysis/json").toString();
  if (!analysis.isEmpty()) {
    try {
      analysis_panel_->FromJson(Json::parse(analysis.toStdString()));
    } catch (const std::exception&) {
    }
  }
  processor_.SetUdp(analysis_panel_->Udp());
  ReconfigureAnalysis();
  std::vector<std::string> trend;
  for (const auto& n : s.value("trend/selected").toStringList()) {
    trend.push_back(n.toStdString());
  }
  trend_panel_->Select(trend);
  trend_panel_->RestoreState(s);
  restoreGeometry(s.value("window/geometry").toByteArray());
  restoreState(s.value("window/state").toByteArray());
}

void MainWindow::SavePreferences() const {
  QSettings s;
  s.setValue("connection/host", QString::fromStdString(address_.host));
  s.setValue("connection/buffer_frames", address_.buffer_frames);
  s.setValue("connection/client_name",
             QString::fromStdString(address_.client_name));
  s.setValue("connection/control_port", address_.control_port);
  s.setValue("connection/data_port", address_.data_port);
  s.setValue("connection/use_ipc", address_.use_ipc);
  s.setValue("connection/ipc_dir", QString::fromStdString(address_.ipc_dir));
  s.setValue("stream_recording/max_fps", stream_fps_->value());
  s.setValue("stream_recording/path", stream_path_->text());
  s.setValue("display/average_frames", average_frames_->value());
  s.setValue("display/colormap", colormap_->currentText());
  s.setValue("display/reverse", reverse_cmap_->isChecked());
  s.setValue("display/scale_mode", scale_mode_->currentIndex());
  s.setValue("display/lo", scale_lo_->value());
  s.setValue("display/hi", scale_hi_->value());
  s.setValue("display/pct_lo", pct_lo_->value());
  s.setValue("display/pct_hi", pct_hi_->value());
  s.setValue("display/log", log_scale_->isChecked());
  s.setValue("display/flip_h", flip_h_->isChecked());
  s.setValue("display/flip_v", flip_v_->isChecked());
  s.setValue("display/rotation", rotation_->currentIndex());
  s.setValue("acquisition/series_frames", arm_frames_->value());
  s.setValue("analysis/json",
             QString::fromStdString(analysis_panel_->ToJson().dump()));
  QStringList trend;
  for (const auto& n : trend_panel_->Selected()) {
    trend << QString::fromStdString(n);
  }
  s.setValue("trend/selected", trend);
  trend_panel_->SaveState(&s);
  s.setValue("window/geometry", saveGeometry());
  s.setValue("window/state", saveState());
}

void MainWindow::closeEvent(QCloseEvent* event) {
  SavePreferences();
  Disconnect();
  event->accept();
}

}  // namespace AndorEMCCDcontroller
