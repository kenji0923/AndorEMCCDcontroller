/**
 * @file ImageIO.cc
 * @brief Implementation of AndorEMCCDcontroller::ImageIO.
 */

#include <AndorEMCCDcontroller/ImageIO.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>

#include <fitsio.h>
#include <png.h>
#include <tiffio.h>

namespace AndorEMCCDcontroller {

namespace {

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

std::string Extension(const std::string& path) {
  const auto dot = path.find_last_of('.');
  const auto slash = path.find_last_of("/\\");
  if (dot == std::string::npos ||
      (slash != std::string::npos && dot < slash)) {
    return "";
  }
  return Lower(path.substr(dot));
}

bool SameShape(const std::vector<Frame>& frames) {
  for (const auto& f : frames) {
    if (f.header.width != frames[0].header.width ||
        f.header.height != frames[0].header.height ||
        f.header.pixel_type != frames[0].header.pixel_type) {
      return false;
    }
  }
  return true;
}

std::string IsoTime(double unix_time) {
  const std::time_t t = static_cast<std::time_t>(unix_time);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
  const int ms = static_cast<int>((unix_time - static_cast<double>(t)) * 1000);
  char out[40];
  std::snprintf(out, sizeof(out), "%s.%03d", buf, std::clamp(ms, 0, 999));
  return out;
}

std::string NumberedPath(const std::string& path, std::size_t index) {
  const auto dot = path.find_last_of('.');
  char suffix[32];
  std::snprintf(suffix, sizeof(suffix), "_%04zu", index);
  if (dot == std::string::npos) return path + suffix;
  return path.substr(0, dot) + suffix + path.substr(dot);
}

/** RAII wrapper for TIFF*. */
struct TiffCloser {
  void operator()(TIFF* t) const {
    if (t) TIFFClose(t);
  }
};

}  // namespace

Json OrientationToJson(const DisplayOrientation& o) {
  return Json{{"rotation_cw_deg", o.rotation_cw_deg},
              {"flip_horizontal", o.flip_horizontal},
              {"flip_vertical", o.flip_vertical},
              {"tiff_orientation", TiffOrientation(o)},
              {"order", "flip, then rotate clockwise"}};
}

DisplayOrientation OrientationFromMetadata(const Json& metadata) {
  DisplayOrientation o;
  if (!metadata.is_object() || !metadata.contains("display")) return o;
  const Json& d = metadata["display"];
  if (!d.is_object()) return o;
  o.rotation_cw_deg = ((d.value("rotation_cw_deg", 0) % 360) + 360) % 360;
  o.flip_horizontal = d.value("flip_horizontal", false);
  o.flip_vertical = d.value("flip_vertical", false);
  return o;
}

int TiffOrientation(const DisplayOrientation& o) {
  // 2x2 matrices on (x, y) with y pointing down; the display is R^n * F.
  struct M {
    int a, b, c, d;  // x' = a x + b y, y' = c x + d y
  };
  auto mul = [](const M& p, const M& q) {
    return M{p.a * q.a + p.b * q.c, p.a * q.b + p.b * q.d,
             p.c * q.a + p.d * q.c, p.c * q.b + p.d * q.d};
  };
  M m{o.flip_horizontal ? -1 : 1, 0, 0, o.flip_vertical ? -1 : 1};
  const M cw{0, -1, 1, 0};  // 90 degrees clockwise on screen
  for (int k = 0; k < (o.rotation_cw_deg / 90) % 4; ++k) m = mul(cw, m);
  // Transform a viewer applies for each Orientation value (EXIF semantics).
  static const M kTable[8] = {{1, 0, 0, 1},  {-1, 0, 0, 1}, {-1, 0, 0, -1},
                              {1, 0, 0, -1}, {0, 1, 1, 0},  {0, -1, 1, 0},
                              {0, -1, -1, 0}, {0, 1, -1, 0}};
  for (int i = 0; i < 8; ++i) {
    const M& k = kTable[i];
    if (k.a == m.a && k.b == m.b && k.c == m.c && k.d == m.d) return i + 1;
  }
  return 1;
}

bool FormatFromPath(const std::string& path, ImageFormat* format) {
  const std::string ext = Extension(path);
  if (ext == ".tif" || ext == ".tiff") {
    *format = ImageFormat::kTiff;
  } else if (ext == ".png") {
    *format = ImageFormat::kPng16;
  } else if (ext == ".fits" || ext == ".fit" || ext == ".fts") {
    *format = ImageFormat::kFits;
  } else if (ext == ".npy") {
    *format = ImageFormat::kNpy;
  } else {
    return false;
  }
  return true;
}

std::vector<std::string> FormatFilters() {
  return {"TIFF 16-bit (*.tif *.tiff)", "FITS (*.fits *.fit)",
          "PNG 16-bit grayscale (*.png)", "NumPy (*.npy)"};
}

bool WriteFrames(const std::string& path, ImageFormat format,
                 const std::vector<Frame>& frames, const Json& metadata,
                 std::string* error) {
  if (frames.empty()) {
    *error = "no frames to save";
    return false;
  }
  switch (format) {
    case ImageFormat::kTiff:
      return WriteTiff(path, frames, metadata, error);
    case ImageFormat::kFits:
      return WriteFits(path, frames, metadata, error);
    case ImageFormat::kNpy:
      if (!WriteNpy(path, frames, error)) return false;
      {
        Json annotations = Json::array();
        for (const auto& frame : frames)
          annotations.push_back(
              {{"frame", frame.header}, {"analysis", frame.analysis}});
        std::ofstream sidecar(path + ".json");
        sidecar << Json{{"metadata", metadata}, {"frames", annotations}}.dump(
            2);
        if (!sidecar) {
          *error = "cannot write metadata: " + path + ".json";
          return false;
        }
      }
      return true;
    case ImageFormat::kPng16:
      if (frames.size() == 1) return WritePng16(path, frames[0], metadata, error);
      for (std::size_t i = 0; i < frames.size(); ++i) {
        if (!WritePng16(NumberedPath(path, i), frames[i], metadata, error)) {
          return false;
        }
      }
      return true;
  }
  return false;
}

bool WriteTiff(const std::string& path, const std::vector<Frame>& frames,
               const Json& metadata, std::string* error) {
  // Classic TIFF is read by everything; BigTIFF only when it must be.
  std::size_t total = 0;
  for (const auto& f : frames) total += f.header.PayloadBytes() + 4096;
  const char* mode = total > (std::size_t{3900} << 20) ? "w8" : "w";
  std::unique_ptr<TIFF, TiffCloser> tif(TIFFOpen(path.c_str(), mode));
  if (!tif) {
    *error = "cannot create " + path;
    return false;
  }
  const int orientation = TiffOrientation(OrientationFromMetadata(metadata));
  for (std::size_t page = 0; page < frames.size(); ++page) {
    const Frame& f = frames[page];
    const FrameHeader& h = f.header;
    const int bytes = static_cast<int>(PixelTypeSize(h.pixel_type));
    TIFF* t = tif.get();
    TIFFSetField(t, TIFFTAG_IMAGEWIDTH, static_cast<std::uint32_t>(h.width));
    TIFFSetField(t, TIFFTAG_IMAGELENGTH, static_cast<std::uint32_t>(h.height));
    TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, static_cast<std::uint16_t>(8 * bytes));
    TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, static_cast<std::uint16_t>(1));
    TIFFSetField(t, TIFFTAG_SAMPLEFORMAT,
                 h.pixel_type == PixelType::kUint16  ? SAMPLEFORMAT_UINT
                 : h.pixel_type == PixelType::kInt32 ? SAMPLEFORMAT_INT
                                                     : SAMPLEFORMAT_IEEEFP);
    TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(t, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(t, TIFFTAG_ROWSPERSTRIP, static_cast<std::uint32_t>(h.height));
    TIFFSetField(t, TIFFTAG_SOFTWARE, "AndorEMCCDcontroller");
    TIFFSetField(t, TIFFTAG_ORIENTATION, static_cast<std::uint16_t>(orientation));
    if (frames.size() > 1) {
      TIFFSetField(t, TIFFTAG_SUBFILETYPE, FILETYPE_PAGE);
      TIFFSetField(t, TIFFTAG_PAGENUMBER, static_cast<std::uint16_t>(page),
                   static_cast<std::uint16_t>(frames.size()));
    }
    Json description{{"frame", h}, {"analysis", f.analysis}};
    if (page == 0 && !metadata.is_null()) description["metadata"] = metadata;
    const std::string text = description.dump();
    TIFFSetField(t, TIFFTAG_IMAGEDESCRIPTION, text.c_str());
    const std::string date = IsoTime(h.timestamp);
    TIFFSetField(t, TIFFTAG_DATETIME, date.c_str());

    const tmsize_t size = static_cast<tmsize_t>(h.PayloadBytes());
    if (!f.data || f.data->size() < h.PayloadBytes() ||
        TIFFWriteEncodedStrip(t, 0, const_cast<std::uint8_t*>(f.data->data()),
                              size) != size) {
      *error = "write failed for page " + std::to_string(page);
      return false;
    }
    if (!TIFFWriteDirectory(t)) {
      *error = "TIFFWriteDirectory failed";
      return false;
    }
  }
  return true;
}

bool ReadTiff(const std::string& path, std::vector<Frame>* frames,
              std::string* error) {
  std::unique_ptr<TIFF, TiffCloser> tif(TIFFOpen(path.c_str(), "r"));
  if (!tif) {
    *error = "cannot open " + path;
    return false;
  }
  TIFF* t = tif.get();
  do {
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    std::uint16_t bits = 16;
    std::uint16_t format = SAMPLEFORMAT_UINT;
    TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(t, TIFFTAG_IMAGELENGTH, &h);
    TIFFGetFieldDefaulted(t, TIFFTAG_BITSPERSAMPLE, &bits);
    TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLEFORMAT, &format);
    Frame f;
    char* desc = nullptr;
    if (TIFFGetField(t, TIFFTAG_IMAGEDESCRIPTION, &desc) && desc) {
      try {
        const Json j = Json::parse(desc);
        if (j.contains("frame")) f.header = j["frame"].get<FrameHeader>();
        f.analysis = j.value("analysis", Json::object());
      } catch (const std::exception&) {
      }
    }
    f.header.width = static_cast<int>(w);
    f.header.height = static_cast<int>(h);
    f.header.pixel_type = bits == 16 ? PixelType::kUint16
                          : format == SAMPLEFORMAT_IEEEFP ? PixelType::kFloat32
                                                          : PixelType::kInt32;
    auto data = std::make_shared<std::vector<std::uint8_t>>(
        f.header.PayloadBytes());
    const tmsize_t scanline = TIFFScanlineSize(t);
    for (std::uint32_t row = 0; row < h; ++row) {
      if (TIFFReadScanline(t, data->data() + row * scanline, row) < 0) {
        *error = "read failed";
        return false;
      }
    }
    f.data = std::move(data);
    frames->push_back(std::move(f));
  } while (TIFFReadDirectory(t));
  return true;
}

namespace {

/**
 * Writes an 8- or 16-bit image with libpng. Only trivially destructible
 * locals live here, so a libpng longjmp cannot skip a destructor.
 * @param rows Packed rows (big endian for 16 bit).
 * @param text Optional zTXt "Description" (may be null).
 */
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4611)  // No destructible locals here.
#endif
bool WritePngRaw(FILE* fp, int width, int height, int bit_depth,
                 int color_type, const std::uint8_t* rows,
                 std::size_t row_bytes, const char* text) {
  png_structp png =
      png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info = png ? png_create_info_struct(png) : nullptr;
  if (!png || !info || setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, &info);
    return false;
  }
  png_init_io(png, fp);
  png_set_IHDR(png, info, static_cast<png_uint_32>(width),
               static_cast<png_uint_32>(height), bit_depth, color_type,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);
  if (text) {
    png_text entry{};
    entry.compression = PNG_TEXT_COMPRESSION_zTXt;
    entry.key = const_cast<char*>("Description");
    entry.text = const_cast<char*>(text);
    png_set_text(png, info, &entry, 1);
  }
  png_write_info(png, info);
  for (int y = 0; y < height; ++y) {
    png_write_row(png, rows + static_cast<std::size_t>(y) * row_bytes);
  }
  png_write_end(png, nullptr);
  png_destroy_write_struct(&png, &info);
  return true;
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

bool WritePngFile(const std::string& path, int width, int height,
                  int bit_depth, int color_type,
                  const std::vector<std::uint8_t>& rows, std::size_t row_bytes,
                  const char* text, std::string* error) {
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) {
    *error = "cannot create " + path;
    return false;
  }
  const bool ok = WritePngRaw(fp, width, height, bit_depth, color_type,
                              rows.data(), row_bytes, text);
  if (std::fclose(fp) != 0 || !ok) {
    *error = "libpng error while writing " + path;
    return false;
  }
  return true;
}

}  // namespace

bool WritePng16(const std::string& path, const Frame& frame,
                const Json& metadata, std::string* error) {
  const FrameHeader& h = frame.header;
  Json description{{"frame", h}, {"analysis", frame.analysis}};
  if (!metadata.is_null()) description["metadata"] = metadata;
  const std::string text = description.dump();

  // PNG is big endian; wide types are clamped to the 16-bit range.
  const std::size_t row_bytes = static_cast<std::size_t>(h.width) * 2;
  std::vector<std::uint8_t> rows(row_bytes * h.height);
  const std::uint8_t* src = frame.data->data();
  for (std::size_t i = 0; i < static_cast<std::size_t>(h.width) * h.height;
       ++i) {
    std::uint16_t v = 0;
    if (h.pixel_type == PixelType::kUint16) {
      std::memcpy(&v, src + 2 * i, 2);
    } else if (h.pixel_type == PixelType::kInt32) {
      std::int32_t w;
      std::memcpy(&w, src + 4 * i, 4);
      v = static_cast<std::uint16_t>(std::clamp<std::int32_t>(w, 0, 65535));
    } else {
      float w;
      std::memcpy(&w, src + 4 * i, 4);
      v = static_cast<std::uint16_t>(std::clamp(w, 0.0f, 65535.0f));
    }
    rows[2 * i] = static_cast<std::uint8_t>(v >> 8);
    rows[2 * i + 1] = static_cast<std::uint8_t>(v & 0xff);
  }
  return WritePngFile(path, h.width, h.height, 16, PNG_COLOR_TYPE_GRAY, rows,
                      row_bytes, text.c_str(), error);
}

bool WritePngRgb(const std::string& path, int width, int height,
                 const std::uint32_t* rgb, std::string* error) {
  const std::size_t n = static_cast<std::size_t>(width) * height;
  std::vector<std::uint8_t> rows(n * 3);
  for (std::size_t i = 0; i < n; ++i) {
    rows[3 * i] = static_cast<std::uint8_t>(rgb[i] >> 16);
    rows[3 * i + 1] = static_cast<std::uint8_t>(rgb[i] >> 8);
    rows[3 * i + 2] = static_cast<std::uint8_t>(rgb[i]);
  }
  return WritePngFile(path, width, height, 8, PNG_COLOR_TYPE_RGB, rows,
                      static_cast<std::size_t>(width) * 3, nullptr, error);
}

bool WriteFits(const std::string& path, const std::vector<Frame>& frames,
               const Json& metadata, std::string* error) {
  if (!SameShape(frames)) {
    *error = "FITS cube needs frames of equal size and type";
    return false;
  }
  const FrameHeader& h = frames[0].header;
  int status = 0;
  fitsfile* fits = nullptr;
  // A leading '!' makes CFITSIO overwrite an existing file.
  const std::string target = "!" + path;
  fits_create_file(&fits, target.c_str(), &status);
  const int bitpix = h.pixel_type == PixelType::kUint16  ? USHORT_IMG
                     : h.pixel_type == PixelType::kInt32 ? LONG_IMG
                                                         : FLOAT_IMG;
  const int datatype = h.pixel_type == PixelType::kUint16  ? TUSHORT
                       : h.pixel_type == PixelType::kInt32 ? TINT
                                                           : TFLOAT;
  long naxes[3] = {h.width, h.height, static_cast<long>(frames.size())};
  fits_create_img(fits, bitpix, frames.size() > 1 ? 3 : 2, naxes, &status);

  double exposure = h.exposure_s;
  int em_gain = h.em_gain;
  double temperature = h.temperature_c;
  int hbin = h.hbin;
  int vbin = h.vbin;
  int x0 = h.x0;
  int y0 = h.y0;
  long long run = static_cast<long long>(h.run_id);
  const std::string date = IsoTime(h.timestamp);
  fits_update_key(fits, TDOUBLE, "EXPTIME", &exposure, "exposure time [s]",
                  &status);
  fits_update_key(fits, TINT, "EMGAIN", &em_gain, "EM gain setting", &status);
  fits_update_key(fits, TDOUBLE, "CCD-TEMP", &temperature,
                  "sensor temperature [C]", &status);
  fits_update_key(fits, TINT, "XBINNING", &hbin, "horizontal binning",
                  &status);
  fits_update_key(fits, TINT, "YBINNING", &vbin, "vertical binning", &status);
  fits_update_key(fits, TINT, "XORGSUBF", &x0, "first column (1-based)",
                  &status);
  fits_update_key(fits, TINT, "YORGSUBF", &y0, "first row (1-based)", &status);
  fits_update_key(fits, TLONGLONG, "RUNID", &run, "backend run id", &status);
  fits_update_key(fits, TSTRING, "DATE-OBS", const_cast<char*>(date.c_str()),
                  "UTC of first frame readout", &status);
  fits_update_key(fits, TSTRING, "INSTRUME",
                  const_cast<char*>("Andor iXon EMCCD"), nullptr, &status);
  // Display orientation of the GUI; the data stay in readout order.
  const DisplayOrientation o = OrientationFromMetadata(metadata);
  int rotation = o.rotation_cw_deg;
  int flip_h = o.flip_horizontal ? 1 : 0;
  int flip_v = o.flip_vertical ? 1 : 0;
  fits_update_key(fits, TINT, "DISPROT", &rotation,
                  "display rotation clockwise [deg], after flips", &status);
  fits_update_key(fits, TLOGICAL, "DISPFLPH", &flip_h,
                  "display flipped horizontally", &status);
  fits_update_key(fits, TLOGICAL, "DISPFLPV", &flip_v,
                  "display flipped vertically", &status);
  {
    Json tagged = metadata.is_object() ? metadata : Json::object();
    tagged["frame_analysis"] = Json::array();
    for (const auto& frame : frames)
      tagged["frame_analysis"].push_back(
          {{"seq", frame.header.seq}, {"analysis", frame.analysis}});
    const std::string json = tagged.dump();
    // Long text goes to COMMENT cards (72 characters each).
    for (std::size_t i = 0; i < json.size(); i += 70) {
      fits_write_comment(fits, json.substr(i, 70).c_str(), &status);
    }
  }

  const long long pixels = static_cast<long long>(h.width) * h.height;
  for (std::size_t k = 0; k < frames.size() && status == 0; ++k) {
    fits_write_img(fits, datatype, 1 + static_cast<long long>(k) * pixels,
                   pixels, const_cast<std::uint8_t*>(frames[k].data->data()),
                   &status);
  }
  fits_close_file(fits, &status);
  if (status != 0) {
    char text[FLEN_STATUS];
    fits_get_errstatus(status, text);
    *error = std::string("CFITSIO: ") + text;
    return false;
  }
  return true;
}

bool WriteNpy(const std::string& path, const std::vector<Frame>& frames,
              std::string* error) {
  if (!SameShape(frames)) {
    *error = "NPY stack needs frames of equal size and type";
    return false;
  }
  const FrameHeader& h = frames[0].header;
  const char* descr = h.pixel_type == PixelType::kUint16  ? "<u2"
                      : h.pixel_type == PixelType::kInt32 ? "<i4"
                                                          : "<f4";
  std::string shape = frames.size() > 1
                          ? "(" + std::to_string(frames.size()) + ", " +
                                std::to_string(h.height) + ", " +
                                std::to_string(h.width) + ")"
                          : "(" + std::to_string(h.height) + ", " +
                                std::to_string(h.width) + ")";
  std::string header = std::string("{'descr': '") + descr +
                       "', 'fortran_order': False, 'shape': " + shape + ", }";
  // Pad so that the data starts at a multiple of 64 bytes (format v1.0).
  const std::size_t preamble = 10;
  const std::size_t total = ((preamble + header.size() + 1 + 63) / 64) * 64;
  header.append(total - preamble - header.size() - 1, ' ');
  header.push_back('\n');

  std::ofstream out(path, std::ios::binary);
  if (!out) {
    *error = "cannot create " + path;
    return false;
  }
  const unsigned char magic[8] = {0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0};
  out.write(reinterpret_cast<const char*>(magic), 8);
  const std::uint16_t len = static_cast<std::uint16_t>(header.size());
  const unsigned char len_le[2] = {static_cast<unsigned char>(len & 0xff),
                                   static_cast<unsigned char>(len >> 8)};
  out.write(reinterpret_cast<const char*>(len_le), 2);
  out.write(header.data(), static_cast<std::streamsize>(header.size()));
  for (const auto& f : frames) {
    out.write(reinterpret_cast<const char*>(f.data->data()),
              static_cast<std::streamsize>(h.PayloadBytes()));
  }
  if (!out) {
    *error = "write failed: " + path;
    return false;
  }
  return true;
}

}  // namespace AndorEMCCDcontroller
