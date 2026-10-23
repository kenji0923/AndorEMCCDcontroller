/**
 * @file MainWindow.h
 * @brief Main window of the iXon EMCCD frontend.
 */

#ifndef AndorEMCCDcontroller__gui_MainWindow_h
#define AndorEMCCDcontroller__gui_MainWindow_h

#include <atomic>
#include <memory>
#include <vector>

#include <QByteArray>
#include <QMainWindow>
#include <QTimer>
#include <zmq.hpp>

#include <AndorEMCCDcontroller/CameraClient.h>
#include <AndorEMCCDcontroller/Colormap.h>
#include <AndorEMCCDcontroller/ImageIO.h>

#include "LiveProcessor.h"
#include <AndorEMCCDcontroller/StreamRecorder.h>

class QAction;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class QLineEdit;
class QPushButton;

namespace AndorEMCCDcontroller {

class AnalysisPanel;
class ColorBar;
class ImageView;
class SettingsPanel;
class TrendPanel;

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

  /** Connects to @p address (also used from the command line). */
  void ConnectTo(const ServerAddress& address);
  void StartStreamRecording(const QString& path, double max_fps);
  /** Last used (or saved) connection parameters. */
  const ServerAddress& Address() const { return address_; }
  /** Requests control unless held already (asks before taking over). */
  void RequestControl() {
    if (!have_control_) ToggleControl();
  }

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  void BuildUi();
  void BuildDisplayDock();
  void BuildStreamRecordingDock();
  void ToggleStreamRecording();
  void UpdateStreamRecordingStatus();
  void ShowConnectDialog();
  void Disconnect();
  /** Sends a request; shows errors in the status bar. */
  bool Request(const std::string& cmd, const Json& args, Json* result,
               int timeout_ms = 5000);
  void ReloadSettings();
  void ApplySettings(const Json& patch);
  void StartLive();
  void AcquireSingle();
  void ArmSeries();
  void StopAcquisition();
  void SoftwareTrigger();
  void ToggleControl();
  void UpdateControl(const Json& control);
  /** Shows the EM gain in effect in the status dock and the toolbar. */
  void UpdateEmGain();
  /** This machine's address as seen on the route to the backend. */
  std::string LocalAddress() const;

  void OnStatus(const Json& status);
  void RenderLatest();
  void UpdateColormap();
  void ReconfigureAnalysis();
  void SaveCurrentFrame();
  void SaveView();
  void RecordFrames();
  void SaveRunFromBackend();
  void SaveFramesTo(std::vector<Frame> frames, const QString& path);
  Json SaveMetadata() const;
  DisplayOrientation Orientation() const;
  void LoadPreferences();
  void SavePreferences() const;
  void ResetWindowLayout();

  QByteArray default_window_state_;

  zmq::context_t context_{1};
  std::unique_ptr<ControlClient> control_;
  std::unique_ptr<FrameSubscriber> subscriber_;
  ServerAddress address_;
  LiveProcessor processor_;
  StreamRecorder stream_recorder_;
  QLineEdit* stream_path_ = nullptr;
  QDoubleSpinBox* stream_fps_ = nullptr;
  QPushButton* stream_browse_ = nullptr;
  QPushButton* stream_toggle_ = nullptr;
  QLabel* stream_status_ = nullptr;
  Json info_ = Json::object();
  Json settings_ = Json::object();
  Json status_ = Json::object();
  double last_status_time_ = 0.0;

  ImageView* view_ = nullptr;
  ColorBar* colorbar_ = nullptr;
  SettingsPanel* settings_panel_ = nullptr;
  AnalysisPanel* analysis_panel_ = nullptr;
  TrendPanel* trend_panel_ = nullptr;

  QComboBox* colormap_ = nullptr;
  QCheckBox* reverse_cmap_ = nullptr;
  QComboBox* scale_mode_ = nullptr;
  QDoubleSpinBox* scale_lo_ = nullptr;
  QDoubleSpinBox* scale_hi_ = nullptr;
  QDoubleSpinBox* pct_lo_ = nullptr;
  QDoubleSpinBox* pct_hi_ = nullptr;
  QCheckBox* log_scale_ = nullptr;
  QCheckBox* threshold_mode_ = nullptr;
  QDoubleSpinBox* threshold_ = nullptr;
  QCheckBox* flip_h_ = nullptr;
  QCheckBox* flip_v_ = nullptr;
  QComboBox* rotation_ = nullptr;  ///< Clockwise, index * 90 degrees.
  QSpinBox* arm_frames_ = nullptr;
  QSpinBox* average_frames_ = nullptr;
  QLabel* average_used_ = nullptr;
  Frame displayed_frame_;

  QLabel* status_state_ = nullptr;
  QLabel* status_temp_ = nullptr;
  QLabel* status_run_ = nullptr;
  QLabel* status_rate_ = nullptr;
  QLabel* status_timing_ = nullptr;
  QLabel* status_em_gain_ = nullptr;
  QLabel* status_error_ = nullptr;
  QLabel* cursor_label_ = nullptr;
  QLabel* connection_label_ = nullptr;

  QAction* draw_roi_action_ = nullptr;
  QAction* control_action_ = nullptr;
  QAction* em_gain_action_ = nullptr;
  std::vector<QAction*> mutating_actions_;
  QLabel* status_control_ = nullptr;
  bool have_control_ = false;
  double last_keepalive_ = 0.0;
  QTimer render_timer_;
  QTimer slow_timer_;
  ColormapTable cmap_{};
  std::uint64_t last_rendered_seq_ = UINT64_MAX;
  std::shared_ptr<const FrameResult> shown_;
  int display_frames_ = 0;
  double display_fps_ = 0.0;
  double display_fps_t0_ = 0.0;
  std::atomic<bool> recording_{false};
};

}  // namespace AndorEMCCDcontroller

#endif
