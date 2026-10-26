#include <AndorEMCCDcontroller/DisplayConversion.h>

#include <cmath>
#include <cstring>
#include <limits>

namespace AndorEMCCDcontroller {
namespace {
double Number(const Json& j, const char* key) {
  const auto it = j.find(key);
  return it != j.end() && it->is_number()
             ? it->get<double>()
             : std::numeric_limits<double>::quiet_NaN();
}
}  // namespace

Json ResolveDisplayConversion(const Json& input_config,
                              const Json& input_calibration,
                              const Json& input_settings,
                              const Json& input_background) {
  const Json empty = Json::object();
  const Json& config = input_config.is_object() ? input_config : empty;
  const Json& calibration =
      input_calibration.is_object() ? input_calibration : empty;
  const Json& settings = input_settings.is_object() ? input_settings : empty;
  const Json& background =
      input_background.is_object() ? input_background : empty;
  const std::string units = config.value("units", "counts");
  const int sdk_mode = settings.value("count_convert_mode", 0);
  const std::string raw_units = sdk_mode == 2   ? "photons"
                                : sdk_mode == 1 ? "electrons"
                                                : "counts";
  Json r{{"requested_units", units},
         {"units", raw_units},
         {"valid", false},
         {"raw_pixels_modified", false},
         {"config", config},
         {"calibration", calibration},
         {"offset", 0.0},
         {"scale", 1.0}};
  auto fail = [&](const std::string& error) {
    r["error"] = error;
    return r;
  };
  if (units != "counts" && units != "electrons" && units != "photons")
    return fail("Unknown display units");
  if (units != "counts" && sdk_mode != 0)
    return fail("Set camera SDK Convert to Counts before software conversion");
  double offset = 0.0;
  double scale = 1.0;
  if (units != "counts") {
    const bool camera = config.value("source", "camera") == "camera";
    const Json& values = camera ? calibration : config;
    if (camera && settings.value("acquisition_mode", "") == "accumulate" &&
        settings.value("number_accumulations", 1) > 1 &&
        !config.value("background_subtract_enabled", false))
      return fail(
          "Use a background ROI or manual baseline for accumulated images");
    if (camera) {
      const Json applied = calibration.value("settings", Json::object());
      for (const char* key :
           {"output_amplifier", "hs_speed_index", "preamp_gain_index",
            "em_gain_mode", "em_gain_enabled", "em_gain", "baseline_clamp"}) {
        if (!applied.contains(key) || !settings.contains(key) ||
            applied[key] != settings[key])
          return fail(
              "Camera calibration is unavailable or settings have changed");
      }
    }
    const double sensitivity = Number(values, "sensitivity_e_per_adu");
    const double gain = Number(values, "em_gain");
    if (!(std::isfinite(sensitivity) && sensitivity > 0 &&
          std::isfinite(gain) && gain > 0))
      return fail(
          "Sensitivity and physical EM gain must be available and positive");
    scale = sensitivity / gain;
    offset = Number(values, "baseline_adu");
    if (units == "photons") {
      const double qe = Number(values, "qe");
      if (!(std::isfinite(qe) && qe > 0 && qe <= 1))
        return fail("Quantum efficiency must be available and between 0 and 1");
      if (camera && std::abs(Number(calibration, "wavelength_nm") -
                             Number(config, "wavelength_nm")) > 0.01)
        return fail("Camera quantum efficiency is for a different wavelength");
      scale /= qe;
    }
  }
  if (config.value("background_subtract_enabled", false)) {
    if (!background.value("valid", false))
      return fail("Select an enabled background ROI within the image");
    // The ROI mean already includes the bias; do not subtract it twice.
    offset = Number(background, "mean");
    r["offset_source"] = "background_roi";
  } else {
    r["offset_source"] = units == "counts" ? "none" : "baseline";
  }
  if (!std::isfinite(offset))
    return fail(
        "Baseline is unavailable; use a background ROI or manual calibration");
  if (!std::isfinite(scale) || scale <= 0)
    return fail("Conversion scale is invalid");
  r["offset"] = offset;
  r["scale"] = scale;
  r["units"] = units == "counts" ? raw_units : units;
  r["formula"] = "(raw - offset) * scale";
  r["valid"] = true;
  return r;
}

Frame ConvertDisplayFrame(const Frame& raw, const Json& conversion) {
  Frame out = raw;
  out.analysis["display_conversion"] = conversion;
  if (!raw.data || !conversion.value("valid", false)) return out;
  const double offset = conversion.at("offset").get<double>();
  const double scale = conversion.at("scale").get<double>();
  out.header.pixel_type = PixelType::kFloat32;
  auto data =
      std::make_shared<std::vector<std::uint8_t>>(out.header.PayloadBytes());
  const std::size_t n =
      static_cast<std::size_t>(raw.header.width) * raw.header.height;
  for (std::size_t i = 0; i < n; ++i) {
    double value = 0;
    if (raw.header.pixel_type == PixelType::kUint16) {
      std::uint16_t v;
      std::memcpy(&v, raw.data->data() + 2 * i, 2);
      value = v;
    } else if (raw.header.pixel_type == PixelType::kInt32) {
      std::int32_t v;
      std::memcpy(&v, raw.data->data() + 4 * i, 4);
      value = v;
    } else {
      float v;
      std::memcpy(&v, raw.data->data() + 4 * i, 4);
      value = v;
    }
    const float converted = static_cast<float>((value - offset) * scale);
    std::memcpy(data->data() + 4 * i, &converted, 4);
  }
  out.data = std::move(data);
  return out;
}
}  // namespace AndorEMCCDcontroller
