#include "ConversionPanel.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QWheelEvent>
#include <cmath>
#include <stdexcept>

namespace AndorEMCCDcontroller {
namespace {
class CalibrationSpinBox : public QDoubleSpinBox {
 public:
  using QDoubleSpinBox::QDoubleSpinBox;

 protected:
  void wheelEvent(QWheelEvent* event) override {
    if (QSettings()
            .value("camera_settings/wheel_changes_enabled", false)
            .toBool())
      QDoubleSpinBox::wheelEvent(event);
    else
      event->ignore();
  }
};
}  // namespace

ConversionPanel::ConversionPanel(QWidget* parent) : QWidget(parent) {
  auto* form = new QFormLayout(this);
  units_ = new QComboBox(this);
  units_->setObjectName("display_units");
  units_->addItems({tr("Counts"), tr("Electrons"), tr("Photons")});
  form->addRow(tr("Display units"), units_);
  source_ = new QComboBox(this);
  source_->setObjectName("display_calibration_source");
  source_->addItems({tr("Camera calibration"), tr("Manual coefficients")});
  form->addRow(tr("Coefficients"), source_);
  auto field = [&](const char* name, const QString& label, double lo, double hi,
                   double initial) {
    auto* spin = new CalibrationSpinBox(this);
    spin->setObjectName(name);
    spin->setDecimals(6);
    spin->setRange(lo, hi);
    spin->setValue(initial);
    form->addRow(label, spin);
    connect(spin, &QDoubleSpinBox::valueChanged, this,
            &ConversionPanel::Changed);
    return spin;
  };
  wavelength_ =
      field("display_wavelength_nm", tr("Wavelength (nm)"), 100, 2000, 780);
  sensitivity_ = field("display_sensitivity", tr("Sensitivity (e⁻/ADU)"),
                       0.000001, 1e6, 1);
  gain_ = field("display_em_gain", tr("Physical EM gain"), 0.000001, 1e6, 1);
  qe_ = field("display_qe", tr("Quantum efficiency (%)"), 0.000001, 100, 100);
  load_qe_ = new QPushButton(tr("Load QE from SDK table"), this);
  load_qe_->setObjectName("load_qe_table");
  load_qe_->setToolTip(
      tr("Read the backend's sensor-model table at the selected "
         "wavelength and switch to editable manual coefficients. "
         "No exposure or camera setting change is needed."));
  form->addRow(load_qe_);
  qe_reference_label_ = new QLabel(this);
  qe_reference_label_->setObjectName("qe_table_status");
  qe_reference_label_->setWordWrap(true);
  form->addRow(qe_reference_label_);
  baseline_ = field("display_baseline", tr("Baseline (ADU)"), -1e9, 1e9, 100);
  auto* hint = new QLabel(
      tr("Electrons = (raw − offset) × sensitivity / EM gain.\n"
         "Photons = electrons / QE. A BG ROI replaces the baseline.\n"
         "Raw saves keep camera counts. Save converted writes float32."),
      this);
  hint->setWordWrap(true);
  form->addRow(hint);
  camera_values_ = new QLabel(this);
  camera_values_->setWordWrap(true);
  form->addRow(camera_values_);
  status_ = new QLabel(this);
  status_->setObjectName("display_conversion_status");
  status_->setWordWrap(true);
  form->addRow(status_);
  connect(units_, &QComboBox::currentIndexChanged, this,
          &ConversionPanel::Changed);
  connect(source_, &QComboBox::currentIndexChanged, this, [this] {
    UpdateSource();
    emit Changed();
  });
  connect(load_qe_, &QPushButton::clicked, this,
          &ConversionPanel::LoadQeTableRequested);
  connect(qe_, &QDoubleSpinBox::valueChanged, this, [this] {
    qe_from_table_ = false;
    UpdateQeReference();
  });
  connect(wavelength_, &QDoubleSpinBox::valueChanged, this,
          &ConversionPanel::UpdateQeReference);
  UpdateSource();
}

Json ConversionPanel::Config() const {
  return Json{{"units", units_->currentIndex() == 2   ? "photons"
                        : units_->currentIndex() == 1 ? "electrons"
                                                      : "counts"},
              {"source", source_->currentIndex() == 0 ? "camera" : "manual"},
              {"wavelength_nm", wavelength_->value()},
              {"sensitivity_e_per_adu", sensitivity_->value()},
              {"em_gain", gain_->value()},
              {"qe", qe_->value() / 100.0},
              {"qe_source", source_->currentIndex() == 0 ? "camera"
                            : qe_from_table_             ? "sdk_qe_table"
                                                         : "manual"},
              {"qe_reference", qe_reference_},
              {"baseline_adu", baseline_->value()}};
}

void ConversionPanel::SetConfig(const Json& config) {
  units_->setCurrentIndex(config.value("units", "counts") == "photons"     ? 2
                          : config.value("units", "counts") == "electrons" ? 1
                                                                           : 0);
  source_->setCurrentIndex(config.value("source", "camera") == "manual" ? 1
                                                                        : 0);
  wavelength_->setValue(config.value("wavelength_nm", 780.0));
  sensitivity_->setValue(config.value("sensitivity_e_per_adu", 1.0));
  gain_->setValue(config.value("em_gain", 1.0));
  qe_->setValue(100 * config.value("qe", 1.0));
  baseline_->setValue(config.value("baseline_adu", 100.0));
  qe_reference_ = config.value("qe_reference", Json::object());
  qe_from_table_ = config.value("qe_source", "manual") == "sdk_qe_table";
  UpdateSource();
}

void ConversionPanel::SetCalibration(const Json& calibration) {
  calibration_ = calibration;
  QString text = tr("Camera values: ");
  const auto show = [&](const char* key) {
    auto it = calibration.find(key);
    return it != calibration.end() && it->is_number()
               ? QString::number(it->get<double>(), 'g', 8)
               : tr("unavailable");
  };
  text +=
      tr("S=%1 e⁻/ADU, EM=%2, QE=%3\n%4")
          .arg(show("sensitivity_e_per_adu"), show("em_gain"), show("qe"),
               QString::fromStdString(calibration.value("em_gain_source", "")));
  camera_values_->setText(text);
  camera_values_->setToolTip(QString::fromStdString(calibration.dump(2)));
  if (source_->currentIndex() == 0) {
    const auto update = [&](QDoubleSpinBox* spin, const char* key,
                            double scale = 1) {
      if (calibration.contains(key) && calibration[key].is_number()) {
        const QSignalBlocker block(spin);
        spin->setValue(scale * calibration[key].get<double>());
      }
    };
    update(sensitivity_, "sensitivity_e_per_adu");
    update(gain_, "em_gain");
    update(qe_, "qe", 100);
    update(baseline_, "baseline_adu");
  }
}

void ConversionPanel::UpdateSource() {
  for (auto* spin : {sensitivity_, gain_, qe_, baseline_})
    spin->setEnabled(source_->currentIndex() != 0);
  if (source_->currentIndex() == 0) {
    qe_from_table_ = false;
    SetCalibration(calibration_);
  }
  UpdateQeReference();
}

void ConversionPanel::ApplyQeTable(const Json& reference) {
  const double qe = reference.at("qe").get<double>();
  const double wavelength = reference.at("wavelength_nm").get<double>();
  if (!std::isfinite(qe) || qe < qe_->minimum() / 100.0 || qe > 1 ||
      !std::isfinite(wavelength) ||
      std::abs(wavelength - wavelength_->value()) > 0.000001)
    throw std::runtime_error(
        "Invalid QE table response or wavelength mismatch");
  // Do not turn unavailable camera coefficients into apparently calibrated
  // manual defaults when automatically switching sources.
  if (source_->currentIndex() == 0) {
    for (const char* key :
         {"sensitivity_e_per_adu", "em_gain", "baseline_adu"}) {
      if (!calibration_.contains(key) || !calibration_[key].is_number() ||
          !std::isfinite(calibration_[key].get<double>()) ||
          (std::string(key) != "baseline_adu" && calibration_[key] <= 0))
        throw std::runtime_error(
            "Camera coefficients are incomplete. Select Manual coefficients "
            "and enter sensitivity, physical EM gain and baseline first.");
    }
  }
  {
    const QSignalBlocker block_source(source_);
    const QSignalBlocker block_qe(qe_);
    source_->setCurrentIndex(1);
    qe_->setValue(100 * qe);
  }
  qe_reference_ = reference;
  qe_from_table_ = true;
  UpdateSource();
  emit Changed();
}

void ConversionPanel::SetQeTableError(const QString& error) {
  qe_reference_label_->setText(tr("QE table: %1").arg(error));
  qe_reference_label_->setStyleSheet("color: #c62828;");
}

void ConversionPanel::UpdateQeReference() {
  qe_reference_label_->setStyleSheet("");
  if (qe_reference_.empty() || !qe_reference_.is_object()) {
    qe_reference_label_->clear();
    return;
  }
  const double wavelength = qe_reference_.value("wavelength_nm", 0.0);
  const bool stale =
      qe_from_table_ && std::abs(wavelength - wavelength_->value()) > 0.000001;
  const QString reference =
      tr("%1: %2% at %3 nm (sensor-model reference)")
          .arg(QString::fromStdString(
              qe_reference_.value("table_file", "SDK table")))
          .arg(100 * qe_reference_.value("qe", 0.0), 0, 'g', 8)
          .arg(wavelength, 0, 'g', 10);
  qe_reference_label_->setToolTip(
      QString::fromStdString(qe_reference_.dump(2)));
  if (source_->currentIndex() == 0) {
    qe_reference_label_->setText(
        tr("Saved table reference (inactive): %1").arg(reference));
  } else if (stale) {
    SetQeTableError(
        tr("Wavelength changed. Reload QE or enter a manual value.\n%1")
            .arg(reference));
  } else {
    qe_reference_label_->setText(
        qe_from_table_
            ? reference
            : tr("Manual QE override. Previous reference: %1").arg(reference));
  }
}

void ConversionPanel::ShowResult(const Json& result) {
  const bool valid = result.value("valid", false);
  status_->setText(
      valid ? tr("Display: %1 (float32)")
                  .arg(QString::fromStdString(result.value("units", "counts")))
            : tr("Showing raw %1: %2")
                  .arg(QString::fromStdString(result.value("units", "counts")),
                       QString::fromStdString(
                           result.value("error", "Calibration unavailable"))));
  status_->setStyleSheet(valid ? "" : "color: #c62828;");
}
}  // namespace AndorEMCCDcontroller
