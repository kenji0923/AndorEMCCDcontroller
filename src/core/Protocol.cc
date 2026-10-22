/**
 * @file Protocol.cc
 * @brief Implementation of AndorEMCCDcontroller::Protocol.
 */

#include <AndorEMCCDcontroller/Protocol.h>

#include <chrono>
#include <stdexcept>

namespace AndorEMCCDcontroller {

std::string PixelTypeName(PixelType type) {
  switch (type) {
    case PixelType::kUint16:
      return "uint16";
    case PixelType::kInt32:
      return "int32";
    case PixelType::kFloat32:
      return "float32";
  }
  return "uint16";
}

PixelType PixelTypeFromName(const std::string& name) {
  if (name == "uint16") return PixelType::kUint16;
  if (name == "int32") return PixelType::kInt32;
  if (name == "float32") return PixelType::kFloat32;
  throw std::invalid_argument("unknown pixel type: " + name);
}

std::size_t PixelTypeSize(PixelType type) {
  return type == PixelType::kUint16 ? 2 : 4;
}

std::size_t FrameHeader::PayloadBytes() const {
  return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
         PixelTypeSize(pixel_type);
}

void to_json(Json& j, const FrameHeader& h) {
  j = Json{{"run_id", h.run_id},
           {"index", h.index},
           {"seq", h.seq},
           {"width", h.width},
           {"height", h.height},
           {"dtype", PixelTypeName(h.pixel_type)},
           {"hbin", h.hbin},
           {"vbin", h.vbin},
           {"x0", h.x0},
           {"y0", h.y0},
           {"exposure_s", h.exposure_s},
           {"em_gain", h.em_gain},
           {"timestamp", h.timestamp},
           {"temperature_c", h.temperature_c}};
  if (h.camera_time_s >= 0.0) {
    j["camera_time_s"] = h.camera_time_s;
  } else {
    j["camera_time_s"] = nullptr;
  }
}

void from_json(const Json& j, FrameHeader& h) {
  h.run_id = j.value("run_id", std::uint64_t{0});
  h.index = j.value("index", std::int64_t{0});
  h.seq = j.value("seq", std::uint64_t{0});
  h.width = j.at("width").get<int>();
  h.height = j.at("height").get<int>();
  h.pixel_type = PixelTypeFromName(j.value("dtype", std::string("uint16")));
  h.hbin = j.value("hbin", 1);
  h.vbin = j.value("vbin", 1);
  h.x0 = j.value("x0", 1);
  h.y0 = j.value("y0", 1);
  h.exposure_s = j.value("exposure_s", 0.0);
  h.em_gain = j.value("em_gain", 0);
  h.timestamp = j.value("timestamp", 0.0);
  h.temperature_c = j.value("temperature_c", 0.0);
  const auto it = j.find("camera_time_s");
  h.camera_time_s =
      (it != j.end() && it->is_number()) ? it->get<double>() : -1.0;
}

double UnixTimeNow() {
  using namespace std::chrono;
  return duration<double>(system_clock::now().time_since_epoch()).count();
}

std::string TcpEndpoint(const std::string& host, int port) {
  return "tcp://" + host + ":" + std::to_string(port);
}

std::string IpcEndpoint(const std::string& dir, const std::string& name) {
  return "ipc://" + dir + "/" + name;
}

}  // namespace AndorEMCCDcontroller
