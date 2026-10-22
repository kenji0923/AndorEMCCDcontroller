/**
 * @file AndorCameraStub.cc
 * @brief MakeAndorCamera() when the backend is built without the Andor SDK.
 */

#include <AndorEMCCDcontroller/Camera.h>

namespace AndorEMCCDcontroller {

std::unique_ptr<Camera> MakeAndorCamera(const std::string& /*config_dir*/) {
  return nullptr;
}

}  // namespace AndorEMCCDcontroller
