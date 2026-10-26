#ifndef AndorEMCCDcontroller__DisplayConversion_h
#define AndorEMCCDcontroller__DisplayConversion_h

#include <AndorEMCCDcontroller/Protocol.h>

namespace AndorEMCCDcontroller {
// Resolve display-only conversion. No raw pixels are changed. The returned
// provenance includes invalid/unavailable coefficients instead of guessing.
Json ResolveDisplayConversion(const Json& config, const Json& calibration,
                              const Json& camera_settings,
                              const Json& background = Json::object());
// Float32 derived frame; invalid conversion leaves the raw payload untouched.
Frame ConvertDisplayFrame(const Frame& raw, const Json& conversion);
}  // namespace AndorEMCCDcontroller
#endif
