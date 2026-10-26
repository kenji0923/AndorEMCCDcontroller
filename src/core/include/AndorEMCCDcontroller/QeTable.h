#ifndef AndorEMCCDcontroller__QeTable_h
#define AndorEMCCDcontroller__QeTable_h

#include <AndorEMCCDcontroller/Protocol.h>

#include <string>

namespace AndorEMCCDcontroller {

// Reads the SDK's sensor-variant table without calling the SDK or acquiring.
// Returns a QE fraction and provenance; throws on missing/invalid data or
// wavelengths outside the table. No manufacturer data is bundled here.
Json ReadSdkQeTable(const std::string& config_dir,
                    const std::string& head_model, double wavelength_nm);

}  // namespace AndorEMCCDcontroller
#endif
