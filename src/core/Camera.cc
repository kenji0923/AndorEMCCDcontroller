/**
 * @file Camera.cc
 * @brief Implementation of AndorEMCCDcontroller::Camera helpers.
 */

#include <AndorEMCCDcontroller/Camera.h>

#include <algorithm>

namespace AndorEMCCDcontroller {

FrameGeometry ComputeGeometry(const CameraSettings& s, int sensor_width,
                              int sensor_height) {
  FrameGeometry g;
  g.hbin = std::max(1, s.hbin);
  g.vbin = std::max(1, s.vbin);
  int hstart = 1;
  int hend = sensor_width;
  int vstart = 1;
  int vend = sensor_height;
  if (s.isolated_crop) {
    // Isolated crop keeps the corner next to the readout register.
    hend = s.crop_width > 0 ? std::min(s.crop_width, sensor_width)
                            : sensor_width;
    vend = s.crop_height > 0 ? std::min(s.crop_height, sensor_height)
                             : sensor_height;
  } else {
    hstart = std::clamp(s.hstart, 1, sensor_width);
    vstart = std::clamp(s.vstart, 1, sensor_height);
    hend = s.hend <= 0 ? sensor_width : std::clamp(s.hend, hstart, sensor_width);
    vend =
        s.vend <= 0 ? sensor_height : std::clamp(s.vend, vstart, sensor_height);
  }
  g.hbin = std::min(g.hbin, hend - hstart + 1);
  g.vbin = std::min(g.vbin, vend - vstart + 1);
  g.width = (hend - hstart + 1) / g.hbin;
  g.height = (vend - vstart + 1) / g.vbin;
  g.x0 = hstart;
  g.y0 = vstart;
  // Accumulated sums and count-converted data can exceed 16 bits.
  const bool wide = (s.acquisition_mode == AcquisitionMode::kAccumulate &&
                     s.number_accumulations > 1) ||
                    s.count_convert_mode != 0;
  g.pixel_type = wide ? PixelType::kInt32 : PixelType::kUint16;
  return g;
}

}  // namespace AndorEMCCDcontroller
