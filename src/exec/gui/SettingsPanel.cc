/**
 * @file SettingsPanel.cc
 * @brief Implementation of AndorEMCCDcontroller::SettingsPanel.
 */

#include "SettingsPanel.h"

#include <cmath>
#include <limits>

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace AndorEMCCDcontroller {

namespace {

const char* kChangedStyle = "background-color: #fff2b3; color: black;";
constexpr char kWheelChangesPreference[] = "camera_settings/wheel_changes_enabled";

template <typename SpinBox>
class WheelControlledSpinBox final : public SpinBox {
 public:
  WheelControlledSpinBox(QCheckBox* wheel_changes, QWidget* parent)
      : SpinBox(parent), wheel_changes_(wheel_changes) {}

 protected:
  void wheelEvent(QWheelEvent* event) override {
    if (wheel_changes_->isChecked()) {
      SpinBox::wheelEvent(event);
    } else {
      // Scroll the panel instead, including when the spin box has focus.
      for (QWidget* parent = this->parentWidget(); parent;
           parent = parent->parentWidget()) {
        if (auto* scroll = qobject_cast<QScrollArea*>(parent)) {
          QWheelEvent forwarded(
              QPointF(scroll->viewport()->mapFromGlobal(
                  event->globalPosition().toPoint())),
              event->globalPosition(), event->pixelDelta(), event->angleDelta(),
              event->buttons(), event->modifiers(), event->phase(),
              event->inverted(), event->source(), event->pointingDevice());
          QCoreApplication::sendEvent(scroll->viewport(), &forwarded);
          event->setAccepted(forwarded.isAccepted());
          return;
        }
      }
      event->ignore();
    }
  }

 private:
  QCheckBox* wheel_changes_;
};

QString Label(const std::string& s) { return QString::fromStdString(s); }

std::vector<std::pair<QString, Json>> NameChoices(const Json& names) {
  std::vector<std::pair<QString, Json>> out;
  for (const auto& n : names) {
    QString text = Label(n.get<std::string>());
    text.replace('_', ' ');
    text[0] = text[0].toUpper();
    out.emplace_back(text, n);
  }
  return out;
}

}  // namespace

SettingsPanel::SettingsPanel(QWidget* parent) : QWidget(parent) { Build(); }

void SettingsPanel::Build() {
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  wheel_changes_ = new QCheckBox(tr("Allow mouse wheel to change values"), this);
  wheel_changes_->setObjectName("camera_settings_wheel_changes_enabled");
  wheel_changes_->setToolTip(
      tr("Enable wheel changes for camera numeric settings, even without "
         "clicking the field. Arrow buttons and keyboard editing always work."));
  wheel_changes_->setChecked(
      QSettings().value(kWheelChangesPreference, false).toBool());
  connect(wheel_changes_, &QCheckBox::toggled, this, [](bool enabled) {
    QSettings().setValue(kWheelChangesPreference, enabled);
  });
  outer->addWidget(wheel_changes_);
  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  auto* content = new QWidget(scroll);
  auto* layout = new QVBoxLayout(content);
  scroll->setWidget(content);
  outer->addWidget(scroll);

  auto group = [&](const QString& title) {
    auto* box = new QGroupBox(title, content);
    auto* form = new QFormLayout(box);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    layout->addWidget(box);
    return form;
  };

  QFormLayout* acq = group(tr("Acquisition"));
  AddField(acq, "acquisition_mode", tr("Mode"), Kind::kChoice);
  AddField(acq, "trigger_mode", tr("Trigger"), Kind::kChoice, 0, 0, 1, 0,
           tr("external: each edge starts an exposure of the set length\n"
              "external_start: one edge starts an internally timed series\n"
              "external_exposure: exposure lasts while the trigger is high\n"
              "software: the 'Software trigger' button starts an exposure"));
  AddField(acq, "exposure_time_s", tr("Exposure (ms)"), Kind::kDouble, 0,
           3.6e6, 1e3, 3);
  AddField(acq, "number_kinetics", tr("Kinetic frames"), Kind::kInt, 1, 1e8);
  AddField(acq, "kinetic_cycle_time_s", tr("Kinetic cycle (ms)"),
           Kind::kDouble, 0, 3.6e6, 1e3, 3, tr("0 = as fast as possible"));
  AddField(acq, "number_accumulations", tr("Accumulations"), Kind::kInt, 1,
           1e6);
  AddField(acq, "accumulation_cycle_time_s", tr("Accumulation cycle (ms)"),
           Kind::kDouble, 0, 3.6e6, 1e3, 3);
  AddField(acq, "frame_transfer", tr("Frame transfer"), Kind::kBool);
  AddField(acq, "fast_external_trigger", tr("Fast external trigger"),
           Kind::kBool, 0, 0, 1, 0,
           tr("Do not wait for a keep-clean cycle before accepting a trigger"));
  AddField(acq, "trigger_invert", tr("Trigger on falling edge"), Kind::kBool);
  AddField(acq, "metadata", tr("Frame time stamps"), Kind::kBool);

  QFormLayout* readout = group(tr("Readout"));
  AddField(readout, "output_amplifier", tr("Amplifier"), Kind::kChoice);
  AddField(readout, "hs_speed_index", tr("Horizontal speed"), Kind::kChoice);
  AddField(readout, "vs_speed_index", tr("Vertical shift"), Kind::kChoice);
  AddField(readout, "vs_amplitude", tr("Vertical clock voltage"),
           Kind::kChoice);
  AddField(readout, "preamp_gain_index", tr("Pre-amp gain"), Kind::kChoice);
  AddField(readout, "baseline_clamp", tr("Baseline clamp"), Kind::kBool);
  AddField(readout, "count_convert_mode", tr("Count convert"), Kind::kChoice,
           0, 0, 1, 0,
           tr("Converts acquired and saved pixel values through the camera SDK. "
              "On iXon, conversion requires baseline clamp, isolated crop off, "
              "EM gain of at least 10, a preamp gain above the lowest setting, "
              "and a supported readout. Photons also uses Wavelength (nm)."));
  AddField(readout, "count_convert_wavelength_nm", tr("Wavelength (nm)"),
           Kind::kDouble, 100, 2000, 1, 1);

  QFormLayout* em = group(tr("EM gain"));
  AddField(em, "em_gain_enabled", tr("EM gain on"), Kind::kBool, 0, 0, 1, 0,
           tr("Off: the register runs at the mode's minimum (0 = off, or "
              "x1 in the linear and real modes). The value below is kept."));
  AddField(em, "em_gain", tr("EM gain"), Kind::kInt, 0, 4095);
  AddField(em, "em_gain_mode", tr("Gain mode"), Kind::kChoice);
  AddField(em, "em_advanced", tr("Advanced (>300x)"), Kind::kBool, 0, 0, 1, 0,
           tr("High EM gain ages the register; avoid strong light."));

  QFormLayout* image = group(tr("Image area"));
  AddField(image, "hbin", tr("Horizontal binning"), Kind::kInt, 1, 4096);
  AddField(image, "vbin", tr("Vertical binning"), Kind::kInt, 1, 4096);
  AddField(image, "hstart", tr("First column"), Kind::kInt, 1, 65535);
  AddField(image, "hend", tr("Last column (0 = edge)"), Kind::kInt, 0, 65535);
  AddField(image, "vstart", tr("First row"), Kind::kInt, 1, 65535);
  AddField(image, "vend", tr("Last row (0 = edge)"), Kind::kInt, 0, 65535);
  AddField(image, "isolated_crop", tr("Isolated crop mode"), Kind::kBool, 0,
           0, 1, 0,
           tr("Reads only a corner of the sensor for high frame rates.\n"
              "Light outside the crop corrupts the data."));
  AddField(image, "crop_width", tr("Crop width"), Kind::kInt, 0, 65535);
  AddField(image, "crop_height", tr("Crop height"), Kind::kInt, 0, 65535);

  QFormLayout* cool = group(tr("Cooling"));
  AddField(cool, "cooler_on", tr("Cooler on"), Kind::kBool);
  AddField(cool, "target_temperature_c", tr("Target (°C)"), Kind::kInt, -150,
           50);
  AddField(cool, "fan_mode", tr("Fan"), Kind::kChoice);
  AddField(cool, "cooler_persist_on_shutdown", tr("Keep cooling on exit"),
           Kind::kBool);

  QFormLayout* shutter = group(tr("External shutter"));
  AddField(shutter, "shutter_mode", tr("Shutter"), Kind::kChoice);
  AddField(shutter, "shutter_ttl_high_open", tr("TTL high opens"),
           Kind::kBool);
  AddField(shutter, "shutter_opening_time_ms", tr("Opening time (ms)"),
           Kind::kInt, 0, 10000);
  AddField(shutter, "shutter_closing_time_ms", tr("Closing time (ms)"),
           Kind::kInt, 0, 10000);
  layout->addStretch();

  auto* buttons = new QHBoxLayout();
  auto* apply = new QPushButton(tr("Apply"), this);
  apply_ = apply;
  auto* revert = new QPushButton(tr("Revert"), this);
  apply->setDefault(true);
  buttons->addWidget(revert);
  buttons->addStretch();
  buttons->addWidget(apply);
  outer->addLayout(buttons);
  connect(apply, &QPushButton::clicked, this,
          [this] { emit ApplyRequested(ChangedSettings()); });
  connect(revert, &QPushButton::clicked, this, [this] {
    SetSettings(settings_);
    emit ReloadRequested();
  });

  SetChoices("acquisition_mode",
             NameChoices(Json::array({"single", "accumulate", "kinetic",
                                      "continuous"})));
  SetChoices("trigger_mode",
             NameChoices(Json::array({"internal", "external", "external_start",
                                      "external_exposure", "software"})));
  SetChoices("fan_mode", NameChoices(Json::array({"full", "low", "off"})));
  SetChoices("shutter_mode",
             NameChoices(Json::array({"auto", "open", "closed"})));
  SetChoices("count_convert_mode",
             {{tr("Counts"), 0}, {tr("Electrons"), 1}, {tr("Photons"), 2}});
  SetChoices("em_gain_mode", {{tr("DAC 0-255"), 0},
                              {tr("DAC 0-4095"), 1},
                              {tr("Linear"), 2},
                              {tr("Real EM gain"), 3}});
  SetChoices("output_amplifier", {{tr("Electron multiplying"), 0},
                                  {tr("Conventional"), 1}});
}

void SettingsPanel::AddField(QFormLayout* form, const std::string& key,
                             const QString& label, Kind kind, double lo,
                             double hi, double scale, int decimals,
                             const QString& tooltip) {
  Field f;
  f.key = key;
  f.kind = kind;
  f.scale = scale;
  switch (kind) {
    case Kind::kBool: {
      auto* w = new QCheckBox(this);
      connect(w, &QCheckBox::toggled, this, &SettingsPanel::MarkChanged);
      f.widget = w;
      break;
    }
    case Kind::kInt: {
      auto* w = new WheelControlledSpinBox<QSpinBox>(wheel_changes_, this);
      w->setRange(static_cast<int>(lo),
                  static_cast<int>(std::min(hi, 2.0e9)));
      w->setKeyboardTracking(false);
      connect(w, &QSpinBox::valueChanged, this, &SettingsPanel::MarkChanged);
      f.widget = w;
      break;
    }
    case Kind::kDouble: {
      auto* w = new WheelControlledSpinBox<QDoubleSpinBox>(wheel_changes_, this);
      w->setDecimals(decimals);
      w->setRange(lo, hi);
      w->setKeyboardTracking(false);
      connect(w, &QDoubleSpinBox::valueChanged, this,
              &SettingsPanel::MarkChanged);
      f.widget = w;
      break;
    }
    case Kind::kChoice: {
      auto* w = new QComboBox(this);
      connect(w, &QComboBox::currentIndexChanged, this, [this, key] {
        if (key == "output_amplifier") UpdateHsChoices();
        if (key == "em_gain_mode") UpdateEmRange();
        MarkChanged();
      });
      f.widget = w;
      break;
    }
  }
  if (!tooltip.isEmpty()) f.widget->setToolTip(tooltip);
  form->addRow(label, f.widget);
  fields_[key] = f;
}

void SettingsPanel::SetChoices(
    const std::string& key,
    const std::vector<std::pair<QString, Json>>& choices) {
  auto it = fields_.find(key);
  if (it == fields_.end()) return;
  auto* combo = qobject_cast<QComboBox*>(it->second.widget);
  const bool was = updating_;
  updating_ = true;
  const Json current = WidgetValue(it->second);
  combo->clear();
  for (const auto& [text, value] : choices) {
    combo->addItem(text, QString::fromStdString(value.dump()));
  }
  if (!current.is_null()) SetWidgetValue(it->second, current);
  updating_ = was;
}

Json SettingsPanel::WidgetValue(const Field& f) const {
  switch (f.kind) {
    case Kind::kBool:
      return qobject_cast<QCheckBox*>(f.widget)->isChecked();
    case Kind::kInt:
      return qobject_cast<QSpinBox*>(f.widget)->value();
    case Kind::kDouble:
      return qobject_cast<QDoubleSpinBox*>(f.widget)->value() / f.scale;
    case Kind::kChoice: {
      auto* combo = qobject_cast<QComboBox*>(f.widget);
      if (combo->currentIndex() < 0) return nullptr;
      return Json::parse(combo->currentData().toString().toStdString());
    }
  }
  return nullptr;
}

void SettingsPanel::SetWidgetValue(const Field& f, const Json& v) {
  switch (f.kind) {
    case Kind::kBool:
      qobject_cast<QCheckBox*>(f.widget)->setChecked(v.get<bool>());
      break;
    case Kind::kInt:
      qobject_cast<QSpinBox*>(f.widget)->setValue(v.get<int>());
      break;
    case Kind::kDouble:
      qobject_cast<QDoubleSpinBox*>(f.widget)->setValue(v.get<double>() *
                                                        f.scale);
      break;
    case Kind::kChoice: {
      auto* combo = qobject_cast<QComboBox*>(f.widget);
      const int i = combo->findData(QString::fromStdString(v.dump()));
      if (i >= 0) combo->setCurrentIndex(i);
      break;
    }
  }
}

void SettingsPanel::SetInfo(const Json& info) {
  info_ = info;
  updating_ = true;
  if (info.contains("trigger_modes")) {
    SetChoices("trigger_mode", NameChoices(info["trigger_modes"]));
  }
  if (info.contains("acquisition_modes")) {
    SetChoices("acquisition_mode", NameChoices(info["acquisition_modes"]));
  }
  std::vector<std::pair<QString, Json>> amps;
  const Json amplifiers = info.value("amplifiers", Json::array());
  for (std::size_t i = 0; i < amplifiers.size(); ++i) {
    amps.emplace_back(Label(amplifiers[i].value("name", std::string("?"))),
                      static_cast<int>(i));
  }
  if (!amps.empty()) SetChoices("output_amplifier", amps);
  UpdateHsChoices();

  const Json vs = info.value("vs_speeds_us", Json::array());
  const int rec = info.value("recommended_vs_speed_index", 0);
  std::vector<std::pair<QString, Json>> vs_choices{
      {tr("Fastest recommended (%1 µs)")
           .arg(rec < static_cast<int>(vs.size()) ? vs[rec].get<double>() : 0.0),
       -1}};
  for (std::size_t i = 0; i < vs.size(); ++i) {
    vs_choices.emplace_back(tr("%1 µs").arg(vs[i].get<double>()),
                            static_cast<int>(i));
  }
  SetChoices("vs_speed_index", vs_choices);

  std::vector<std::pair<QString, Json>> vsa;
  const Json amplitudes = info.value("vs_amplitudes", Json::array());
  for (std::size_t i = 0; i < amplitudes.size(); ++i) {
    vsa.emplace_back(Label(amplitudes[i].get<std::string>()),
                     static_cast<int>(i));
  }
  if (vsa.empty()) vsa.emplace_back(tr("Normal"), 0);
  SetChoices("vs_amplitude", vsa);

  std::vector<std::pair<QString, Json>> pa;
  const Json gains = info.value("preamp_gains", Json::array());
  for (std::size_t i = 0; i < gains.size(); ++i) {
    pa.emplace_back(tr("%1x").arg(gains[i].get<double>()), static_cast<int>(i));
  }
  if (pa.empty()) pa.emplace_back(tr("1x"), 0);
  SetChoices("preamp_gain_index", pa);

  const Json temps = info.value("temperature_range_c", Json::array({-100, 20}));
  qobject_cast<QSpinBox*>(fields_["target_temperature_c"].widget)
      ->setRange(temps[0].get<int>(), temps[1].get<int>());
  const int w = info.value("sensor_width", 512);
  const int h = info.value("sensor_height", 512);
  for (const char* key : {"hstart", "hend", "crop_width"}) {
    qobject_cast<QSpinBox*>(fields_[key].widget)
        ->setRange(std::string(key) == "hstart" ? 1 : 0, w);
  }
  for (const char* key : {"vstart", "vend", "crop_height"}) {
    qobject_cast<QSpinBox*>(fields_[key].widget)
        ->setRange(std::string(key) == "vstart" ? 1 : 0, h);
  }
  fields_["isolated_crop"].widget->setEnabled(
      info.value("has_isolated_crop", true));
  fields_["count_convert_mode"].widget->setEnabled(
      info.value("has_count_convert", true));
  fields_["frame_transfer"].widget->setEnabled(
      info.value("has_frame_transfer", true));
  UpdateEmRange();
  updating_ = false;
  if (!settings_.empty()) SetSettings(settings_);
}

void SettingsPanel::UpdateHsChoices() {
  const Json amplifiers = info_.value("amplifiers", Json::array());
  const Json amp = WidgetValue(fields_["output_amplifier"]);
  const int index = amp.is_number() ? amp.get<int>() : 0;
  std::vector<std::pair<QString, Json>> hs;
  if (index >= 0 && index < static_cast<int>(amplifiers.size())) {
    const Json speeds = amplifiers[index].value("hs_speeds_mhz", Json::array());
    for (std::size_t i = 0; i < speeds.size(); ++i) {
      hs.emplace_back(tr("%1 MHz").arg(speeds[i].get<double>()),
                      static_cast<int>(i));
    }
  }
  if (hs.empty()) hs.emplace_back(tr("Index 0"), 0);
  SetChoices("hs_speed_index", hs);
}

void SettingsPanel::UpdateEmRange() {
  // The SDK range depends on the gain mode; the backend reports the range
  // for the applied mode, so only widen it here.
  const Json range = info_.value("em_gain_range", Json::array({0, 255}));
  const Json mode = WidgetValue(fields_["em_gain_mode"]);
  int hi = range[1].get<int>();
  if (mode.is_number() && mode.get<int>() == 1) hi = std::max(hi, 4095);
  if (mode.is_number() && mode.get<int>() >= 2) hi = std::max(hi, 1000);
  qobject_cast<QSpinBox*>(fields_["em_gain"].widget)->setRange(0, hi);
}

void SettingsPanel::SetSettings(const Json& settings) {
  settings_ = settings;
  updating_ = true;
  // The amplifier decides the HS speed list, so set it first.
  if (settings.contains("output_amplifier")) {
    SetWidgetValue(fields_["output_amplifier"], settings["output_amplifier"]);
    UpdateHsChoices();
  }
  for (auto& [key, field] : fields_) {
    if (settings.contains(key)) SetWidgetValue(field, settings[key]);
  }
  updating_ = false;
  MarkChanged();
}

Json SettingsPanel::ChangedSettings() const {
  Json patch = Json::object();
  for (const auto& [key, field] : fields_) {
    const Json v = WidgetValue(field);
    if (v.is_null()) continue;
    const auto it = settings_.find(key);
    bool same = it != settings_.end() && *it == v;
    if (!same && it != settings_.end() && it->is_number() && v.is_number()) {
      same = std::abs(it->get<double>() - v.get<double>()) <=
             1e-9 * std::max(1.0, std::abs(v.get<double>()));
    }
    if (!same) patch[key] = v;
  }
  return patch;
}

void SettingsPanel::SetEditable(bool editable, const QString& reason) {
  apply_->setEnabled(editable);
  apply_->setToolTip(editable ? QString() : reason);
}

void SettingsPanel::MarkChanged() {
  if (updating_) return;
  const Json patch = ChangedSettings();
  for (auto& [key, field] : fields_) {
    field.widget->setStyleSheet(patch.contains(key) ? kChangedStyle : "");
  }
}

}  // namespace AndorEMCCDcontroller
