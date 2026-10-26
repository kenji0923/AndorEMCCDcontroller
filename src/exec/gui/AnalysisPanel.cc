/**
 * @file AnalysisPanel.cc
 * @brief Implementation of AndorEMCCDcontroller::AnalysisPanel/TrendPanel.
 */

#include "AnalysisPanel.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace AndorEMCCDcontroller {

namespace {

enum RoiColumn { kName, kX, kY, kW, kH, kOn, kBackground, kFirstStat };
constexpr int kStatColumns = 5;  // sum max min mean std
enum FormulaColumn { kFName, kFFormula, kFValue };
enum UdpColumn { kUOn, kUVariable, kUHost, kUPort, kUFormat };
// qtconsole listens on 9000 by default; further rows count up from there.
constexpr int kFirstUdpPort = 9000;

const std::uint32_t kPalette[] = {0xff4040, 0x40c0ff, 0x60ff60, 0xffc020,
                                  0xff60ff, 0x40ffe0, 0xffffff, 0xa080ff};

QString FormatValue(double v) {
  if (!std::isfinite(v)) return QStringLiteral("nan");
  return QString::number(v, 'g', 7);
}

QTableWidgetItem* ReadOnly(const QString& text) {
  auto* item = new QTableWidgetItem(text);
  item->setFlags(item->flags() & ~Qt::ItemIsEditable);
  return item;
}

}  // namespace

AnalysisPanel::AnalysisPanel(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);

  // ROIs.
  auto* roi_box = new QGroupBox(tr("Regions of interest"), this);
  auto* roi_layout = new QVBoxLayout(roi_box);
  rois_ = new QTableWidget(0, kFirstStat + kStatColumns, roi_box);
  rois_->setObjectName("analysis_rois");
  rois_->setHorizontalHeaderLabels(
      {tr("Name"), tr("X"), tr("Y"), tr("W"), tr("H"), tr("On"), tr("BG"),
       tr("Sum"), tr("Max"), tr("Min"), tr("Mean"), tr("Std")});
  rois_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  rois_->verticalHeader()->hide();
  rois_->setSelectionBehavior(QAbstractItemView::SelectRows);
  rois_->setSelectionMode(QAbstractItemView::SingleSelection);
  rois_->setMinimumHeight(130);
  roi_layout->addWidget(rois_);
  auto* roi_buttons = new QHBoxLayout();
  auto* add_roi = new QPushButton(tr("Add"), roi_box);
  auto* draw_roi = new QPushButton(tr("Draw on image"), roi_box);
  draw_roi->setCheckable(true);
  auto* remove_roi = new QPushButton(tr("Remove"), roi_box);
  roi_buttons->addWidget(add_roi);
  roi_buttons->addWidget(draw_roi);
  roi_buttons->addWidget(remove_roi);
  roi_buttons->addStretch();
  roi_layout->addLayout(roi_buttons);

  // Formulas.
  auto* f_box = new QGroupBox(tr("Formulas"), this);
  auto* f_layout = new QVBoxLayout(f_box);
  formulas_ = new QTableWidget(0, 3, f_box);
  formulas_->setHorizontalHeaderLabels({tr("Name"), tr("Formula"), tr("Value")});
  formulas_->horizontalHeader()->setSectionResizeMode(kFFormula,
                                                      QHeaderView::Stretch);
  formulas_->verticalHeader()->hide();
  formulas_->setMinimumHeight(110);
  f_layout->addWidget(formulas_);
  variables_ = new QLabel(f_box);
  variables_->setWordWrap(true);
  variables_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  f_layout->addWidget(variables_);
  auto* f_buttons = new QHBoxLayout();
  auto* add_f = new QPushButton(tr("Add"), f_box);
  auto* remove_f = new QPushButton(tr("Remove"), f_box);
  f_buttons->addWidget(add_f);
  f_buttons->addWidget(remove_f);
  f_buttons->addStretch();
  f_layout->addLayout(f_buttons);

  // UDP output: one value per datagram, one variable per row.
  auto* udp_box = new QGroupBox(tr("UDP output (one value per datagram)"), this);
  auto* udp_layout = new QVBoxLayout(udp_box);
  udp_ = new QTableWidget(0, 5, udp_box);
  udp_->setHorizontalHeaderLabels(
      {tr("On"), tr("Variable"), tr("Host"), tr("Port"), tr("Format")});
  udp_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  // Variable names need the room; the host column takes what is left.
  udp_->horizontalHeader()->setSectionResizeMode(kUVariable,
                                                 QHeaderView::Interactive);
  udp_->setColumnWidth(kUVariable, 105);
  udp_->horizontalHeader()->setSectionResizeMode(kUHost, QHeaderView::Stretch);
  udp_->verticalHeader()->hide();
  udp_->setSelectionBehavior(QAbstractItemView::SelectRows);
  udp_->setSelectionMode(QAbstractItemView::SingleSelection);
  udp_->setMinimumHeight(115);
  udp_->setToolTip(
      tr("Each row sends one variable per frame to host:port as a text number "
         "(or an 8-byte little-endian double), e.g. for\n"
         "qtconsole --protocol udp --port 9000 --measurement roi1_sum"));
  udp_layout->addWidget(udp_);
  auto* udp_buttons = new QHBoxLayout();
  auto* add_udp = new QPushButton(tr("Add"), udp_box);
  auto* remove_udp = new QPushButton(tr("Remove"), udp_box);
  udp_status_ = new QLabel(udp_box);
  udp_status_->setWordWrap(true);
  udp_buttons->addWidget(add_udp);
  udp_buttons->addWidget(remove_udp);
  udp_buttons->addWidget(udp_status_, 1);
  udp_layout->addLayout(udp_buttons);

  layout->addWidget(roi_box, 3);
  layout->addWidget(f_box, 2);
  layout->addWidget(udp_box, 1);

  connect(add_roi, &QPushButton::clicked, this,
          [this] { AddRoi(QRect(206, 206, 100, 100)); });
  connect(remove_roi, &QPushButton::clicked, this, [this] {
    const int row = rois_->currentRow();
    if (row < 0) return;
    rois_->removeRow(row);
    UpdateVariableHelp();
    emit ConfigChanged();
  });
  connect(draw_roi, &QPushButton::toggled, this, &AnalysisPanel::DrawRoiToggled);
  connect(rois_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
    if (updating_ || item->column() >= kFirstStat) return;
    if (item->column() == kBackground && item->checkState() == Qt::Checked) {
      updating_ = true;
      for (int row = 0; row < rois_->rowCount(); ++row) {
        if (row != item->row())
          rois_->item(row, kBackground)->setCheckState(Qt::Unchecked);
      }
      updating_ = false;
    }
    UpdateVariableHelp();
    emit ConfigChanged();
  });
  connect(rois_, &QTableWidget::currentCellChanged, this,
          [this] { emit ConfigChanged(); });
  connect(add_f, &QPushButton::clicked, this, [this] {
    const auto rois = Rois();
    const std::string hint =
        rois.size() >= 2 ? rois[0].name + "_sum - " + rois[1].name + "_sum"
        : rois.size() == 1 ? rois[0].name + "_mean"
                           : "1";
    AddFormulaRow({"f" + std::to_string(formulas_->rowCount() + 1), hint});
    emit ConfigChanged();
  });
  connect(remove_f, &QPushButton::clicked, this, [this] {
    const int row = formulas_->currentRow();
    if (row < 0) return;
    formulas_->removeRow(row);
    emit ConfigChanged();
  });
  connect(formulas_, &QTableWidget::itemChanged, this,
          [this](QTableWidgetItem* item) {
            if (updating_ || item->column() == kFValue) return;
            emit ConfigChanged();
          });
  connect(add_udp, &QPushButton::clicked, this, [this] {
    UdpChannel c;
    // Default to the last formula (usually the derived signal), else the
    // first variable.
    const auto formulas = Formulas();
    if (!formulas.empty()) {
      c.variable = formulas.back().name;
    } else if (!variable_names_.isEmpty()) {
      c.variable = variable_names_.front().toStdString();
    }
    int port = kFirstUdpPort;
    for (const auto& u : Udp().channels) port = std::max(port, u.port + 1);
    c.port = static_cast<quint16>(std::min(port, 65535));
    AddUdpRow(c);
    emit UdpChanged();
  });
  connect(remove_udp, &QPushButton::clicked, this, [this] {
    const int row = udp_->currentRow();
    if (row < 0) return;
    udp_->removeRow(row);
    emit UdpChanged();
  });
  connect(udp_, &QTableWidget::itemChanged, this, [this] {
    if (!updating_) emit UdpChanged();
  });
  UpdateVariableHelp();
}

void AnalysisPanel::AddRoiRow(const Roi& roi) {
  updating_ = true;
  const int row = rois_->rowCount();
  rois_->insertRow(row);
  auto* name = new QTableWidgetItem(QString::fromStdString(roi.name));
  name->setForeground(QColor((roi.color >> 16) & 0xff, (roi.color >> 8) & 0xff,
                             roi.color & 0xff));
  name->setData(Qt::UserRole, static_cast<uint>(roi.color));
  rois_->setItem(row, kName, name);
  rois_->setItem(row, kX, new QTableWidgetItem(QString::number(roi.x)));
  rois_->setItem(row, kY, new QTableWidgetItem(QString::number(roi.y)));
  rois_->setItem(row, kW, new QTableWidgetItem(QString::number(roi.width)));
  rois_->setItem(row, kH, new QTableWidgetItem(QString::number(roi.height)));
  auto* on = new QTableWidgetItem();
  on->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled |
               Qt::ItemIsSelectable);
  on->setCheckState(roi.enabled ? Qt::Checked : Qt::Unchecked);
  rois_->setItem(row, kOn, on);
  auto* background = new QTableWidgetItem();
  background->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled |
                       Qt::ItemIsSelectable);
  background->setCheckState(roi.background ? Qt::Checked : Qt::Unchecked);
  background->setToolTip(
      tr("Use this ROI's mean as the background per pixel. Select one enabled "
         "ROI."));
  rois_->setItem(row, kBackground, background);
  for (int c = 0; c < kStatColumns; ++c) {
    rois_->setItem(row, kFirstStat + c, ReadOnly(QString()));
  }
  updating_ = false;
}

void AnalysisPanel::AddRoi(const QRect& rect) {
  // Pick an unused name roiN.
  const auto existing = Rois();
  int n = static_cast<int>(existing.size()) + 1;
  auto taken = [&](const std::string& name) {
    return std::any_of(existing.begin(), existing.end(),
                       [&](const Roi& r) { return r.name == name; });
  };
  while (taken("roi" + std::to_string(n))) ++n;
  Roi roi;
  roi.name = "roi" + std::to_string(n);
  roi.x = rect.x();
  roi.y = rect.y();
  roi.width = rect.width();
  roi.height = rect.height();
  roi.color = kPalette[next_color_++ % std::size(kPalette)];
  AddRoiRow(roi);
  rois_->setCurrentCell(rois_->rowCount() - 1, kName);
  UpdateVariableHelp();
  emit ConfigChanged();
}

void AnalysisPanel::MoveRoi(int index, int x, int y) {
  if (index < 0 || index >= rois_->rowCount()) return;
  updating_ = true;
  rois_->item(index, kX)->setText(QString::number(x));
  rois_->item(index, kY)->setText(QString::number(y));
  updating_ = false;
  emit ConfigChanged();
}

void AnalysisPanel::SelectRoi(int index) {
  if (index >= 0 && index < rois_->rowCount()) {
    rois_->setCurrentCell(index, kName);
  }
}

int AnalysisPanel::SelectedRoi() const { return rois_->currentRow(); }

std::vector<Roi> AnalysisPanel::Rois() const {
  std::vector<Roi> out;
  for (int row = 0; row < rois_->rowCount(); ++row) {
    Roi r;
    r.name = rois_->item(row, kName)->text().trimmed().toStdString();
    r.x = rois_->item(row, kX)->text().toInt();
    r.y = rois_->item(row, kY)->text().toInt();
    r.width = std::max(1, rois_->item(row, kW)->text().toInt());
    r.height = std::max(1, rois_->item(row, kH)->text().toInt());
    r.enabled = rois_->item(row, kOn)->checkState() == Qt::Checked &&
                IsValidIdentifier(r.name);
    r.color = rois_->item(row, kName)->data(Qt::UserRole).toUInt();
    r.background = rois_->item(row, kBackground)->checkState() == Qt::Checked;
    out.push_back(r);
  }
  return out;
}

std::vector<ExpressionDef> AnalysisPanel::Formulas() const {
  std::vector<ExpressionDef> out;
  for (int row = 0; row < formulas_->rowCount(); ++row) {
    out.push_back({formulas_->item(row, kFName)->text().trimmed().toStdString(),
                   formulas_->item(row, kFFormula)->text().toStdString()});
  }
  return out;
}

UdpConfig AnalysisPanel::Udp() const {
  UdpConfig config;
  for (int row = 0; row < udp_->rowCount(); ++row) {
    UdpChannel c;
    c.enabled = udp_->item(row, kUOn)->checkState() == Qt::Checked;
    c.variable = static_cast<QComboBox*>(udp_->cellWidget(row, kUVariable))
                     ->currentText()
                     .trimmed()
                     .toStdString();
    c.host = udp_->item(row, kUHost)->text().trimmed();
    c.port = static_cast<quint16>(
        std::clamp(udp_->item(row, kUPort)->text().toInt(), 1, 65535));
    c.binary = static_cast<QComboBox*>(udp_->cellWidget(row, kUFormat))
                   ->currentIndex() == 1;
    config.channels.push_back(c);
  }
  return config;
}

void AnalysisPanel::AddUdpRow(const UdpChannel& channel) {
  updating_ = true;
  const int row = udp_->rowCount();
  udp_->insertRow(row);
  auto* on = new QTableWidgetItem();
  on->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled |
               Qt::ItemIsSelectable);
  on->setCheckState(channel.enabled ? Qt::Checked : Qt::Unchecked);
  udp_->setItem(row, kUOn, on);
  // Editable so that a name survives while its ROI/formula is being edited.
  auto* variable = new QComboBox(udp_);
  variable->setEditable(true);
  variable->addItems(variable_names_);
  variable->setCurrentText(QString::fromStdString(channel.variable));
  udp_->setCellWidget(row, kUVariable, variable);
  udp_->setItem(row, kUHost, new QTableWidgetItem(channel.host));
  udp_->setItem(row, kUPort,
                new QTableWidgetItem(QString::number(channel.port)));
  auto* format = new QComboBox(udp_);
  format->addItems({tr("Text"), tr("Double")});
  format->setToolTip(tr("Text number, or 8-byte little-endian double"));
  format->setCurrentIndex(channel.binary ? 1 : 0);
  udp_->setCellWidget(row, kUFormat, format);
  connect(variable, &QComboBox::currentTextChanged, this, [this] {
    if (!updating_) emit UdpChanged();
  });
  connect(format, &QComboBox::currentIndexChanged, this, [this] {
    if (!updating_) emit UdpChanged();
  });
  updating_ = false;
}

void AnalysisPanel::SetVariableNames(const std::vector<std::string>& names) {
  variable_names_.clear();
  for (const auto& n : names) variable_names_ << QString::fromStdString(n);
  updating_ = true;
  for (int row = 0; row < udp_->rowCount(); ++row) {
    auto* combo = static_cast<QComboBox*>(udp_->cellWidget(row, kUVariable));
    const QString current = combo->currentText();
    combo->clear();
    combo->addItems(variable_names_);
    combo->setCurrentText(current);
  }
  updating_ = false;
}

void AnalysisPanel::AddFormulaRow(const ExpressionDef& def) {
  updating_ = true;
  const int row = formulas_->rowCount();
  formulas_->insertRow(row);
  formulas_->setItem(row, kFName,
                     new QTableWidgetItem(QString::fromStdString(def.name)));
  formulas_->setItem(row, kFFormula,
                     new QTableWidgetItem(QString::fromStdString(def.formula)));
  formulas_->setItem(row, kFValue, ReadOnly(QString()));
  updating_ = false;
}

void AnalysisPanel::SetFormulaErrors(const std::vector<std::string>& errors) {
  updating_ = true;
  for (int row = 0; row < formulas_->rowCount(); ++row) {
    const std::string e =
        row < static_cast<int>(errors.size()) ? errors[row] : std::string();
    auto* item = formulas_->item(row, kFFormula);
    item->setToolTip(QString::fromStdString(e));
    item->setForeground(e.empty() ? palette().color(QPalette::Text)
                                  : QColor(220, 40, 40));
    if (!e.empty()) formulas_->item(row, kFValue)->setText(tr("error"));
  }
  updating_ = false;
}

void AnalysisPanel::ShowValues(const std::vector<std::string>& names,
                               const std::vector<double>& values) {
  std::map<std::string, double> lookup;
  for (std::size_t i = 0; i < names.size() && i < values.size(); ++i) {
    lookup[names[i]] = values[i];
  }
  updating_ = true;
  static const char* stats[kStatColumns] = {"sum", "max", "min", "mean", "std"};
  for (int row = 0; row < rois_->rowCount(); ++row) {
    const std::string name = rois_->item(row, kName)->text().toStdString();
    for (int c = 0; c < kStatColumns; ++c) {
      const auto it = lookup.find(name + "_" + stats[c]);
      rois_->item(row, kFirstStat + c)
          ->setText(it == lookup.end() ? QString() : FormatValue(it->second));
    }
  }
  for (int row = 0; row < formulas_->rowCount(); ++row) {
    const auto it =
        lookup.find(formulas_->item(row, kFName)->text().trimmed().toStdString());
    if (it != lookup.end()) {
      formulas_->item(row, kFValue)->setText(FormatValue(it->second));
    }
  }
  updating_ = false;
}

void AnalysisPanel::SetUdpStatus(const QString& text) {
  udp_status_->setText(text);
}

void AnalysisPanel::UpdateVariableHelp() {
  QStringList names;
  for (const auto& roi : Rois()) {
    if (!IsValidIdentifier(roi.name)) continue;
    names << QString::fromStdString(roi.name) + "_{sum,max,min,mean,std,count}";
  }
  variables_->setText(
      tr("Variables: %1. Earlier formulas can be used by name. Functions: "
         "abs sqrt exp ln log10 pow sin cos ...")
          .arg(names.isEmpty() ? tr("(add a ROI)") : names.join(", ")));
}

Json AnalysisPanel::ToJson() const {
  Json formulas = Json::array();
  for (const auto& f : Formulas()) {
    formulas.push_back({{"name", f.name}, {"formula", f.formula}});
  }
  Json rois = Json::array();
  for (int row = 0; row < rois_->rowCount(); ++row) {
    Roi r = Rois()[row];
    r.enabled = rois_->item(row, kOn)->checkState() == Qt::Checked;
    rois.push_back(r);
  }
  Json udp = Json::array();
  for (const auto& c : Udp().channels) {
    udp.push_back({{"enabled", c.enabled},
                   {"variable", c.variable},
                   {"host", c.host.toStdString()},
                   {"port", c.port},
                   {"format", c.binary ? "double" : "text"}});
  }
  return Json{{"rois", rois}, {"formulas", formulas}, {"udp_outputs", udp}};
}

void AnalysisPanel::FromJson(const Json& j) {
  rois_->setRowCount(0);
  formulas_->setRowCount(0);
  for (const auto& r : j.value("rois", Json::array())) {
    AddRoiRow(r.get<Roi>());
    ++next_color_;
  }
  for (const auto& f : j.value("formulas", Json::array())) {
    AddFormulaRow({f.value("name", std::string("f")),
                   f.value("formula", std::string("0"))});
  }
  udp_->setRowCount(0);
  for (const auto& u : j.value("udp_outputs", Json::array())) {
    UdpChannel c;
    c.enabled = u.value("enabled", true);
    c.variable = u.value("variable", std::string());
    c.host = QString::fromStdString(u.value("host", std::string("127.0.0.1")));
    c.port = u.value("port", static_cast<quint16>(kFirstUdpPort));
    c.binary = u.value("format", std::string("text")) == "double";
    AddUdpRow(c);
  }
  // Settings from before per-variable outputs had one JSON target; keep its
  // host:port for the last formula (disabled, since the format changed).
  const Json legacy = j.value("udp", Json::object());
  if (!j.contains("udp_outputs") && legacy.contains("host")) {
    UdpChannel c;
    c.enabled = false;
    const Json fs = j.value("formulas", Json::array());
    if (!fs.empty()) c.variable = fs.back().value("name", std::string());
    c.host = QString::fromStdString(legacy.value("host", std::string()));
    c.port = legacy.value("port", static_cast<quint16>(kFirstUdpPort));
    AddUdpRow(c);
  }
  UpdateVariableHelp();
  emit ConfigChanged();
  emit UdpChanged();
}

TrendPanel::TrendPanel(QWidget* parent) : QWidget(parent) {
  auto* layout = new QHBoxLayout(this);
  auto* side = new QVBoxLayout();
  list_ = new QListWidget(this);
  list_->setMaximumWidth(180);
  window_s_ = new QDoubleSpinBox(this);
  window_s_->setRange(1, 3600);
  window_s_->setValue(30);
  window_s_->setSuffix(tr(" s"));
  auto* clear = new QPushButton(tr("Clear"), this);
  normalize_ = new QCheckBox(tr("Normalize"), this);
  fixed_ = new QCheckBox(tr("Fixed scale"), this);
  fixed_->setToolTip(tr("Fix the value axis to Minimum..Maximum "
                        "(0..1 when normalized); otherwise it follows the data."));
  lo_ = new QDoubleSpinBox(this);
  hi_ = new QDoubleSpinBox(this);
  for (auto* s : {lo_, hi_}) {
    s->setRange(-1e12, 1e12);
    s->setDecimals(3);
    s->setKeyboardTracking(false);
    s->setMaximumWidth(180);
  }
  lo_->setPrefix(tr("Min "));
  hi_->setPrefix(tr("Max "));
  hi_->setValue(1.0);
  side->addWidget(list_);
  side->addWidget(window_s_);
  side->addWidget(normalize_);
  side->addWidget(fixed_);
  side->addWidget(lo_);
  side->addWidget(hi_);
  side->addWidget(clear);
  plot_ = new TrendPlot(this);
  layout->addLayout(side);
  layout->addWidget(plot_, 1);
  connect(clear, &QPushButton::clicked, this, [this] {
    plot_->SetData({}, {});
    emit ClearRequested();
  });
  connect(normalize_, &QCheckBox::toggled, plot_, &TrendPlot::SetNormalize);
  connect(fixed_, &QCheckBox::toggled, this, &TrendPanel::ApplyScale);
  connect(lo_, &QDoubleSpinBox::valueChanged, this, &TrendPanel::ApplyScale);
  connect(hi_, &QDoubleSpinBox::valueChanged, this, &TrendPanel::ApplyScale);
  ApplyScale();
}

void TrendPanel::ApplyScale() {
  lo_->setEnabled(fixed_->isChecked());
  hi_->setEnabled(fixed_->isChecked());
  plot_->SetFixedRange(fixed_->isChecked(), lo_->value(), hi_->value());
}

Json TrendPanel::ToJson() const {
  return Json{{"selected", Selected()}, {"window_s", window_s_->value()},
              {"normalize", normalize_->isChecked()}, {"fixed", fixed_->isChecked()},
              {"lo", lo_->value()}, {"hi", hi_->value()}};
}

void TrendPanel::SaveState(QSettings* s) const {
  s->setValue("trend/window_s", window_s_->value());
  s->setValue("trend/normalize", normalize_->isChecked());
  s->setValue("trend/fixed", fixed_->isChecked());
  s->setValue("trend/lo", lo_->value());
  s->setValue("trend/hi", hi_->value());
}

void TrendPanel::RestoreState(const QSettings& s) {
  window_s_->setValue(s.value("trend/window_s", 30.0).toDouble());
  normalize_->setChecked(s.value("trend/normalize", false).toBool());
  lo_->setValue(s.value("trend/lo", 0.0).toDouble());
  hi_->setValue(s.value("trend/hi", 1.0).toDouble());
  fixed_->setChecked(s.value("trend/fixed", false).toBool());
  ApplyScale();
}

void TrendPanel::SetNames(const std::vector<std::string>& names) {
  const auto selected = Selected();
  names_ = names;
  list_->clear();
  for (const auto& n : names) {
    auto* item = new QListWidgetItem(QString::fromStdString(n), list_);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
  }
  Select(selected);
}

std::vector<std::string> TrendPanel::Selected() const {
  std::vector<std::string> out;
  for (int i = 0; i < list_->count(); ++i) {
    if (list_->item(i)->checkState() == Qt::Checked) {
      out.push_back(list_->item(i)->text().toStdString());
    }
  }
  return out;
}

void TrendPanel::Select(const std::vector<std::string>& names) {
  for (int i = 0; i < list_->count(); ++i) {
    const std::string n = list_->item(i)->text().toStdString();
    if (std::find(names.begin(), names.end(), n) != names.end()) {
      list_->item(i)->setCheckState(Qt::Checked);
    }
  }
}

void TrendPanel::Refresh(const LiveProcessor& processor) {
  if (!isVisible()) return;
  std::vector<double> times;
  std::vector<std::vector<double>> rows;
  processor.History(&times, &rows, 20000);
  std::vector<TrendPlot::Series> series;
  static const QColor colors[] = {QColor(31, 119, 180), QColor(255, 127, 14),
                                  QColor(44, 160, 44),  QColor(214, 39, 40),
                                  QColor(148, 103, 189), QColor(140, 86, 75)};
  if (!times.empty()) {
    // Keep only the requested time window.
    const double t_end = times.back();
    std::size_t first = 0;
    while (first < times.size() && times[first] < t_end - window_s_->value()) {
      ++first;
    }
    times.erase(times.begin(), times.begin() + static_cast<long>(first));
    rows.erase(rows.begin(), rows.begin() + static_cast<long>(first));
    int k = 0;
    for (const auto& name : Selected()) {
      const auto it = std::find(names_.begin(), names_.end(), name);
      if (it == names_.end()) continue;
      const std::size_t col = static_cast<std::size_t>(it - names_.begin());
      TrendPlot::Series s;
      s.name = QString::fromStdString(name);
      s.color = colors[k++ % 6];
      for (const auto& r : rows) {
        s.values.push_back(col < r.size() ? r[col] : std::nan(""));
      }
      series.push_back(std::move(s));
    }
  }
  plot_->SetData(std::move(times), std::move(series));
  double lo = 0.0;
  double hi = 0.0;
  if (!fixed_->isChecked() && plot_->AutoRange(&lo, &hi)) {
    // Mirror the automatic range so that switching to fixed keeps it.
    const QSignalBlocker block_lo(lo_);
    const QSignalBlocker block_hi(hi_);
    lo_->setValue(lo);
    hi_->setValue(hi);
  }
}

}  // namespace AndorEMCCDcontroller
