/**
 * @file Protocol.h
 * @brief Wire protocol shared by the backend, the Qt frontend and the
 *        Python client.
 *
 * Two ZeroMQ channels are used:
 *  - control: ROUTER (backend) <-> REQ/DEALER (client). A request is
 *    [empty, json] and the reply is [empty, json, binary...].
 *  - per-client stream: stream_open/stream_read/stream_close on control,
 *    with bounded history, batch acknowledgements and explicit loss reports.
 *  - legacy data: PUB (backend) -> SUB (client). Each message is
 *    [topic, json, binary?] with topic "frame" or "status".
 *
 * Both channels listen on TCP for remote clients and on IPC (Unix domain
 * socket) for clients on the same host.
 */

#ifndef AndorEMCCDcontroller__Protocol_h
#define AndorEMCCDcontroller__Protocol_h

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace AndorEMCCDcontroller {

using Json = nlohmann::json;

inline constexpr int kProtocolVersion = 1;
inline constexpr int kDefaultControlPort = 47100;
inline constexpr int kDefaultDataPort = 47101;
inline constexpr const char* kDefaultIpcDir = "/run/andor-emccd";
inline constexpr const char* kControlIpcName = "control";
inline constexpr const char* kDataIpcName = "data";
inline constexpr const char* kTopicFrame = "frame";
inline constexpr const char* kTopicStatus = "status";

/** Pixel storage type of a frame payload (little endian, row major). */
enum class PixelType { kUint16, kInt32, kFloat32 };

std::string PixelTypeName(PixelType type);
PixelType PixelTypeFromName(const std::string& name);
std::size_t PixelTypeSize(PixelType type);

/**
 * @brief Metadata sent in front of every frame payload.
 *
 * Coordinates follow the SDK convention: 1-based, first pixel of the payload
 * is (x0, y0) on the sensor and each payload pixel covers hbin x vbin sensor
 * pixels.
 */
struct FrameHeader {
  std::uint64_t run_id = 0;     ///< Acquisition run (one StartAcquisition).
  std::int64_t index = 0;       ///< 0-based frame index within the run.
  std::uint64_t seq = 0;        ///< Global frame counter of the backend.
  int width = 0;
  int height = 0;
  PixelType pixel_type = PixelType::kUint16;
  int hbin = 1;
  int vbin = 1;
  int x0 = 1;
  int y0 = 1;
  double exposure_s = 0.0;
  int em_gain = 0;
  double timestamp = 0.0;       ///< Unix time when the backend read it out.
  double camera_time_s = -1.0;  ///< Time from run start (metadata), <0 n/a.
  double temperature_c = 0.0;

  Json camera_settings = Json::object();  ///< Actual settings for this frame.

  std::size_t PayloadBytes() const;
};

void to_json(Json& j, const FrameHeader& h);
void from_json(const Json& j, FrameHeader& h);

/** A frame with its payload. The payload is immutable and shared. */
struct Frame {
  FrameHeader header;
  std::shared_ptr<const std::vector<std::uint8_t>> data;
  Json delivery = Json::object();  ///< Stream loss report, including zero losses.
  Json analysis =
      Json::object();  ///< Client-side measurements; raw pixels unchanged.
};

/** Seconds since the Unix epoch. */
double UnixTimeNow();

/** "tcp://host:port". */
std::string TcpEndpoint(const std::string& host, int port);
/** "ipc://<dir>/<name>". */
std::string IpcEndpoint(const std::string& dir, const std::string& name);

}  // namespace AndorEMCCDcontroller

#endif
