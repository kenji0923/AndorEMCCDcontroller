#include <AndorEMCCDcontroller/QeTable.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace AndorEMCCDcontroller {

Json ReadSdkQeTable(const std::string& config_dir,
                    const std::string& head_model, double wavelength_nm) {
  if (!std::isfinite(wavelength_nm) || wavelength_nm <= 0)
    throw std::runtime_error("QE wavelength must be finite and positive");
  // GetHeadModel reports e.g. DU897_BV. Only the finish suffix selects a
  // table: never interpret a model or a client argument as a file path.
  const auto separator = head_model.rfind('_');
  const std::string variant =
      separator == std::string::npos ? "" : head_model.substr(separator + 1);
  if (variant.empty() || variant.size() > 16 ||
      variant.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") !=
          std::string::npos)
    throw std::runtime_error("No SDK QE table mapping for head model " +
                             head_model);
  auto path = std::filesystem::path(config_dir) / (variant + ".txt");
  if (!std::filesystem::exists(path)) path.replace_extension(".TXT");
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("SDK QE table not found for " + head_model +
                             " in " + config_dir);
  if (std::filesystem::file_size(path) > 1024 * 1024)
    throw std::runtime_error("SDK QE table exceeds 1 MiB");
  std::vector<std::pair<double, double>> points;
  std::string line;
  while (std::getline(input, line)) {
    line = line.substr(0, line.find('#'));
    std::istringstream row(line);
    row.imbue(std::locale::classic());
    row >> std::ws;
    if (row.eof()) continue;
    double wavelength = 0, qe = 0;
    std::string extra;
    if (!(row >> wavelength >> qe) || (row >> extra) ||
        !std::isfinite(wavelength) || wavelength <= 0 || !std::isfinite(qe) ||
        qe < 0 || qe > 1 ||
        (!points.empty() && wavelength <= points.back().first))
      throw std::runtime_error(
          "Invalid SDK QE table: expected increasing wavelengths and QE "
          "fractions from 0 to 1");
    points.emplace_back(wavelength, qe);
  }
  if (input.bad() || points.size() < 2)
    throw std::runtime_error("SDK QE table is incomplete or unreadable");
  if (wavelength_nm < points.front().first ||
      wavelength_nm > points.back().first)
    throw std::runtime_error("Wavelength is outside the SDK QE table range (" +
                             std::to_string(points.front().first) + ".." +
                             std::to_string(points.back().first) +
                             " nm); no extrapolation");
  const auto upper =
      std::lower_bound(points.begin(), points.end(), wavelength_nm,
                       [](const auto& point, double wavelength) {
                         return point.first < wavelength;
                       });
  const auto lower = upper->first == wavelength_nm ? upper : std::prev(upper);
  const double weight = upper == lower ? 0
                                       : (wavelength_nm - lower->first) /
                                             (upper->first - lower->first);
  const double qe = lower->second + weight * (upper->second - lower->second);
  if (qe <= 0)
    throw std::runtime_error(
        "SDK table QE is zero at this wavelength; "
        "photon conversion is undefined");
  return Json{{"source", "sdk_qe_table"},
              {"calibration_kind",
               "sensor-model reference, not individual calibration"},
              {"head_model", head_model},
              {"sensor_variant", variant},
              {"table_file", path.filename().string()},
              {"table_path", path.string()},
              {"wavelength_nm", wavelength_nm},
              {"qe", qe},
              {"qe_units", "fraction"},
              {"interpolation", "linear"},
              {"range_nm", {points.front().first, points.back().first}},
              {"bracketing_points",
               {{lower->first, lower->second}, {upper->first, upper->second}}}};
}

}  // namespace AndorEMCCDcontroller
