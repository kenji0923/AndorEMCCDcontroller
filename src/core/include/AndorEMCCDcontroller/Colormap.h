/**
 * @file Colormap.h
 * @brief Standard colormaps as 256-entry lookup tables.
 */

#ifndef AndorEMCCDcontroller__Colormap_h
#define AndorEMCCDcontroller__Colormap_h

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace AndorEMCCDcontroller {

/** 256 colors as 0xRRGGBB. */
using ColormapTable = std::array<std::uint32_t, 256>;

/** gray, viridis, inferno, magma, plasma, cividis, turbo, hot, jet, ... */
const std::vector<std::string>& ColormapNames();

/** Table for @p name (gray if unknown); @p reversed flips it. */
ColormapTable GetColormap(const std::string& name, bool reversed = false);

}  // namespace AndorEMCCDcontroller

#endif
