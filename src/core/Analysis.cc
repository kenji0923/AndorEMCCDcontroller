/**
 * @file Analysis.cc
 * @brief Implementation of AndorEMCCDcontroller::Analysis.
 */

#include <AndorEMCCDcontroller/Analysis.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>

namespace AndorEMCCDcontroller {

namespace {

template <typename T>
T Load(const std::uint8_t* data, std::size_t i) {
  T v;
  std::memcpy(&v, data + i * sizeof(T), sizeof(T));
  return v;
}

/** Calls f(value) for each pixel of the clipped rectangle. */
template <typename T, typename F>
void ForEachPixel(const FrameHeader& h, const std::uint8_t* data, int x0,
                  int y0, int x1, int y1, F&& f) {
  for (int y = y0; y < y1; ++y) {
    const std::size_t row = static_cast<std::size_t>(y) * h.width;
    for (int x = x0; x < x1; ++x) f(static_cast<double>(Load<T>(data, row + x)));
  }
}

template <typename F>
void Dispatch(const FrameHeader& h, const std::uint8_t* data, int x0, int y0,
              int x1, int y1, F&& f) {
  switch (h.pixel_type) {
    case PixelType::kUint16:
      ForEachPixel<std::uint16_t>(h, data, x0, y0, x1, y1, f);
      break;
    case PixelType::kInt32:
      ForEachPixel<std::int32_t>(h, data, x0, y0, x1, y1, f);
      break;
    case PixelType::kFloat32:
      ForEachPixel<float>(h, data, x0, y0, x1, y1, f);
      break;
  }
}

}  // namespace

void to_json(Json& j, const Roi& r) {
  j = Json{{"name", r.name},     {"x", r.x},
           {"y", r.y},           {"width", r.width},
           {"height", r.height}, {"enabled", r.enabled},
           {"color", r.color},   {"background", r.background}};
}

void from_json(const Json& j, Roi& r) {
  r.name = j.value("name", std::string("roi"));
  r.x = j.value("x", 0);
  r.y = j.value("y", 0);
  r.width = j.value("width", 1);
  r.height = j.value("height", 1);
  r.enabled = j.value("enabled", true);
  r.color = j.value("color", 0xff4040u);
  r.background = j.value("background", false);
}

Json MeasureBackground(const Frame& frame, const std::vector<Roi>& rois) {
  for (const auto& roi : rois) {
    if (!roi.enabled || !roi.background) continue;
    const auto stats = ComputeRoiStats(
        frame.header, frame.data ? frame.data->data() : nullptr, roi);
    return Json{{"roi", roi},
                {"estimator", "mean"},
                {"sum", stats.sum},
                {"mean", stats.mean},
                {"count", stats.count},
                {"valid", stats.count > 0 && std::isfinite(stats.mean)},
                {"raw_pixels_modified", false}};
  }
  return Json::object();
}

Frame SubtractBackground(const Frame& frame, double background) {
  Frame out = frame;
  if (!frame.data || !std::isfinite(background)) return out;
  out.header.pixel_type = PixelType::kFloat32;
  auto pixels =
      std::make_shared<std::vector<std::uint8_t>>(out.header.PayloadBytes());
  for (int y = 0; y < frame.header.height; ++y) {
    for (int x = 0; x < frame.header.width; ++x) {
      const float value = static_cast<float>(
          PixelValue(frame.header, frame.data->data(), x, y) - background);
      const auto index = static_cast<std::size_t>(y) * frame.header.width + x;
      std::memcpy(pixels->data() + index * sizeof(value), &value,
                  sizeof(value));
    }
  }
  out.data = std::move(pixels);
  out.analysis["background_subtracted"] = true;
  return out;
}

const std::vector<std::string>& StatNames() {
  static const std::vector<std::string> names{"sum", "max",  "min",
                                              "mean", "std", "count"};
  return names;
}

std::vector<double> StatValues(const RoiStats& s) {
  return {s.sum, s.max, s.min, s.mean, s.std, static_cast<double>(s.count)};
}

bool IsValidIdentifier(const std::string& name) {
  static const std::set<std::string> reserved{
      "abs",  "acos", "asin", "atan", "atan2", "ceil", "cos",   "cosh",
      "e",    "exp",  "fac",  "floor", "ln",   "log",  "log10", "ncr",
      "npr",  "pi",   "pow",  "sin",  "sinh",  "sqrt", "tan",   "tanh"};
  if (name.empty() || !std::isalpha(static_cast<unsigned char>(name[0]))) {
    return false;
  }
  for (char c : name) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
  }
  return reserved.count(name) == 0;
}

RoiStats ComputeRoiStats(const FrameHeader& header, const std::uint8_t* data,
                         const Roi& roi) {
  RoiStats s;
  const int x0 = std::clamp(roi.x, 0, header.width);
  const int y0 = std::clamp(roi.y, 0, header.height);
  const int x1 = std::clamp(roi.x + roi.width, 0, header.width);
  const int y1 = std::clamp(roi.y + roi.height, 0, header.height);
  if (x1 <= x0 || y1 <= y0 || data == nullptr) {
    s.min = s.max = s.mean = s.std = std::numeric_limits<double>::quiet_NaN();
    return s;
  }
  // Shifted sums (by the first pixel) keep the variance numerically stable.
  const double shift = PixelValue(header, data, x0, y0);
  double sum = 0.0;
  double sum_sq = 0.0;
  double lo = std::numeric_limits<double>::infinity();
  double hi = -std::numeric_limits<double>::infinity();
  Dispatch(header, data, x0, y0, x1, y1, [&](double v) {
    const double d = v - shift;
    sum += d;
    sum_sq += d * d;
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  });
  s.count = static_cast<std::int64_t>(x1 - x0) * (y1 - y0);
  const double n = static_cast<double>(s.count);
  s.sum = sum + shift * n;
  s.mean = s.sum / n;
  const double mean_d = sum / n;
  s.std = std::sqrt(std::max(0.0, sum_sq / n - mean_d * mean_d));
  s.min = lo;
  s.max = hi;
  return s;
}

double PixelValue(const FrameHeader& header, const std::uint8_t* data, int x,
                  int y) {
  if (x < 0 || y < 0 || x >= header.width || y >= header.height || !data) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const std::size_t i = static_cast<std::size_t>(y) * header.width + x;
  switch (header.pixel_type) {
    case PixelType::kUint16:
      return Load<std::uint16_t>(data, i);
    case PixelType::kInt32:
      return Load<std::int32_t>(data, i);
    case PixelType::kFloat32:
      return Load<float>(data, i);
  }
  return 0.0;
}

void FrameMinMax(const FrameHeader& header, const std::uint8_t* data,
                 double* lo, double* hi) {
  *lo = std::numeric_limits<double>::infinity();
  *hi = -std::numeric_limits<double>::infinity();
  Dispatch(header, data, 0, 0, header.width, header.height, [&](double v) {
    *lo = std::min(*lo, v);
    *hi = std::max(*hi, v);
  });
  if (*lo > *hi) *lo = *hi = 0.0;
}

void FramePercentiles(const FrameHeader& header, const std::uint8_t* data,
                      double lower_pct, double upper_pct, double* lo,
                      double* hi) {
  const std::size_t n = static_cast<std::size_t>(header.width) * header.height;
  if (n == 0) {
    *lo = *hi = 0.0;
    return;
  }
  const double qlo = std::clamp(lower_pct, 0.0, 100.0) / 100.0;
  const double qhi = std::clamp(upper_pct, 0.0, 100.0) / 100.0;
  if (header.pixel_type == PixelType::kUint16) {
    std::vector<std::uint32_t> hist(65536, 0);
    for (std::size_t i = 0; i < n; ++i) ++hist[Load<std::uint16_t>(data, i)];
    const auto find = [&](double q) {
      const double target = q * static_cast<double>(n - 1);
      double cumulative = 0.0;
      for (std::size_t v = 0; v < hist.size(); ++v) {
        cumulative += hist[v];
        if (cumulative > target) return static_cast<double>(v);
      }
      return 65535.0;
    };
    *lo = find(qlo);
    *hi = find(qhi);
    return;
  }
  // Sample at most 65536 pixels for the wide types.
  const std::size_t step = std::max<std::size_t>(1, n / 65536);
  std::vector<double> samples;
  samples.reserve(n / step + 1);
  for (std::size_t i = 0; i < n; i += step) {
    samples.push_back(PixelValue(header, data, static_cast<int>(i % header.width),
                                 static_cast<int>(i / header.width)));
  }
  const auto at = [&](double q) {
    const std::size_t k = static_cast<std::size_t>(q * (samples.size() - 1));
    std::nth_element(samples.begin(), samples.begin() + k, samples.end());
    return samples[k];
  };
  *lo = at(qlo);
  *hi = at(qhi);
}

void MapToIndices(const FrameHeader& header, const std::uint8_t* data,
                  double lo, double hi, bool log, std::uint8_t* out) {
  const std::size_t n = static_cast<std::size_t>(header.width) * header.height;
  if (!(hi > lo)) hi = lo + 1.0;
  if (header.pixel_type == PixelType::kUint16 && !log) {
    // Lookup table over the 16-bit range: one pass, no float math per pixel.
    std::vector<std::uint8_t> lut(65536);
    const double scale = 255.0 / (hi - lo);
    for (int v = 0; v < 65536; ++v) {
      lut[v] = static_cast<std::uint8_t>(
          std::clamp((v - lo) * scale + 0.5, 0.0, 255.0));
    }
    for (std::size_t i = 0; i < n; ++i) out[i] = lut[Load<std::uint16_t>(data, i)];
    return;
  }
  const double log_lo = std::log1p(0.0);
  const double log_span = std::log1p(hi - lo) - log_lo;
  for (std::size_t i = 0; i < n; ++i) {
    const double v = PixelValue(header, data, static_cast<int>(i % header.width),
                                static_cast<int>(i / header.width));
    double t = log ? (std::log1p(std::max(0.0, v - lo)) - log_lo) / log_span
                   : (v - lo) / (hi - lo);
    out[i] = static_cast<std::uint8_t>(std::clamp(t * 255.0 + 0.5, 0.0, 255.0));
  }
}

void MapThresholdToIndices(const FrameHeader& header, const std::uint8_t* data,
                          double threshold, std::uint8_t* out) {
  std::size_t i = 0;
  Dispatch(header, data, 0, 0, header.width, header.height, [&](double value) {
    out[i++] = std::isfinite(value) && value >= threshold ? 255 : 0;
  });
}

}  // namespace AndorEMCCDcontroller
