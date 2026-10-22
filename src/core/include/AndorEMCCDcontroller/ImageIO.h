/**
 * @file ImageIO.h
 * @brief Writing raw frames to TIFF, PNG, FITS and NumPy files.
 *
 * Pixel rows are written in payload order (row 0 = first row read out by the
 * SDK); no flipping is applied. Multi-frame files are written when the
 * format allows it (TIFF pages, FITS cube, NPY 3-D array).
 */

#ifndef AndorEMCCDcontroller__ImageIO_h
#define AndorEMCCDcontroller__ImageIO_h

#include <cstdint>
#include <string>
#include <vector>

#include <AndorEMCCDcontroller/Protocol.h>

namespace AndorEMCCDcontroller {

enum class ImageFormat {
  kTiff,    ///< 16/32-bit, one page per frame, JSON metadata in description.
  kPng16,   ///< 16-bit grayscale, one file per frame.
  kFits,    ///< 2-D image or 3-D cube with exposure/gain keywords.
  kNpy,     ///< NumPy array (h, w) or (n, h, w).
};

/** Guesses the format from the extension (.tif/.tiff/.png/.fits/.fit/.npy). */
bool FormatFromPath(const std::string& path, ImageFormat* format);

/** File dialog filter strings, e.g. "TIFF (*.tif *.tiff)". */
std::vector<std::string> FormatFilters();

/**
 * @brief Writes @p frames to @p path.
 *
 * @param metadata Extra JSON stored with the data (camera settings, ROI...).
 *        For PNG with several frames, "_NNNN" is appended to each file name.
 */
bool WriteFrames(const std::string& path, ImageFormat format,
                 const std::vector<Frame>& frames, const Json& metadata,
                 std::string* error);

bool WriteTiff(const std::string& path, const std::vector<Frame>& frames,
               const Json& metadata, std::string* error);
bool WritePng16(const std::string& path, const Frame& frame,
                const Json& metadata, std::string* error);
bool WriteFits(const std::string& path, const std::vector<Frame>& frames,
               const Json& metadata, std::string* error);
bool WriteNpy(const std::string& path, const std::vector<Frame>& frames,
              std::string* error);

/** 8-bit RGB PNG of a rendered (false color) image, 0xRRGGBB pixels. */
bool WritePngRgb(const std::string& path, int width, int height,
                 const std::uint32_t* rgb, std::string* error);

/** Reads a TIFF written by WriteTiff() (for tests and reloading). */
bool ReadTiff(const std::string& path, std::vector<Frame>* frames,
              std::string* error);

}  // namespace AndorEMCCDcontroller

#endif
