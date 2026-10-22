/**
 * @file Analysis.h
 * @brief ROI statistics and display mapping of raw frames.
 */

#ifndef AndorEMCCDcontroller__Analysis_h
#define AndorEMCCDcontroller__Analysis_h

#include <cstdint>
#include <string>
#include <vector>

#include <AndorEMCCDcontroller/Protocol.h>

namespace AndorEMCCDcontroller {

/** Rectangle in frame pixel coordinates (0-based, payload pixels). */
struct Roi {
  std::string name;  ///< Identifier used in expressions, e.g. "sig".
  int x = 0;
  int y = 0;
  int width = 1;
  int height = 1;
  bool enabled = true;
  std::uint32_t color = 0xff4040;  ///< 0xRRGGBB, for drawing.
};

void to_json(Json& j, const Roi& r);
void from_json(const Json& j, Roi& r);

/** Statistics over the pixels of a ROI (clipped to the frame). */
struct RoiStats {
  double sum = 0.0;
  double max = 0.0;
  double min = 0.0;
  double mean = 0.0;
  double std = 0.0;  ///< Population standard deviation.
  std::int64_t count = 0;
};

/** Names of the per-ROI statistics, in the order of StatValues(). */
const std::vector<std::string>& StatNames();

/** {sum, max, min, mean, std, count}. */
std::vector<double> StatValues(const RoiStats& s);

/** True for [A-Za-z][A-Za-z0-9_]* that is not a built-in function name. */
bool IsValidIdentifier(const std::string& name);

RoiStats ComputeRoiStats(const FrameHeader& header, const std::uint8_t* data,
                         const Roi& roi);

/** Pixel value at (x, y), or NaN outside the frame. */
double PixelValue(const FrameHeader& header, const std::uint8_t* data, int x,
                  int y);

/** Min and max of a whole frame. */
void FrameMinMax(const FrameHeader& header, const std::uint8_t* data,
                 double* lo, double* hi);

/**
 * @brief Values at the given lower/upper percentiles (0..100).
 *
 * Uses a 65536-bin histogram for uint16 data and sampling otherwise.
 */
void FramePercentiles(const FrameHeader& header, const std::uint8_t* data,
                      double lower_pct, double upper_pct, double* lo,
                      double* hi);

/**
 * @brief Maps pixels to 0..255 colormap indices.
 *
 * @param out width * height bytes, row major.
 * @param log Logarithmic scale between lo and hi.
 */
void MapToIndices(const FrameHeader& header, const std::uint8_t* data,
                  double lo, double hi, bool log, std::uint8_t* out);

}  // namespace AndorEMCCDcontroller

#endif
