#include <AndorEMCCDcontroller/StreamRecorder.h>
#include <AndorEMCCDcontroller/ImageIO.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <tiffio.h>

namespace AndorEMCCDcontroller {
namespace {
// Supply CRT I/O explicitly: Windows libtiff may interpret TIFFFdOpen's
// integer as an OS HANDLE rather than a CRT descriptor. Own the FILE until
// after TIFFClose so failed initialization cannot double-close a descriptor.
tmsize_t ReadFile(thandle_t handle, void* data, tmsize_t size) {
  return static_cast<tmsize_t>(std::fread(data, 1,
                                          static_cast<std::size_t>(size),
                                          static_cast<std::FILE*>(handle)));
}
tmsize_t WriteFile(thandle_t handle, void* data, tmsize_t size) {
  return static_cast<tmsize_t>(std::fwrite(data, 1,
                                           static_cast<std::size_t>(size),
                                           static_cast<std::FILE*>(handle)));
}
toff_t SeekFile(thandle_t handle, toff_t offset, int origin) {
  auto* file = static_cast<std::FILE*>(handle);
#ifdef _WIN32
  if (_fseeki64(file, static_cast<__int64>(offset), origin))
    return static_cast<toff_t>(-1);
  return static_cast<toff_t>(_ftelli64(file));
#else
  if (fseeko(file, static_cast<off_t>(offset), origin))
    return static_cast<toff_t>(-1);
  return static_cast<toff_t>(ftello(file));
#endif
}
int KeepFileOpen(thandle_t) { return 0; }
toff_t FileSize(thandle_t handle) {
  const auto position = SeekFile(handle, 0, SEEK_CUR);
  const auto size = SeekFile(handle, 0, SEEK_END);
  SeekFile(handle, position, SEEK_SET);
  return size;
}
int NoMap(thandle_t, void**, toff_t*) { return 0; }
void NoUnmap(thandle_t, void*, toff_t) {}

void MergeLoss(Json* target, const Json& loss) {
  const auto count = loss.value("count", std::uint64_t{0});
  if (!count) return;
  const auto old = target->value("count", std::uint64_t{0});
  if (!old)
    *target = loss;
  else {
    (*target)["last_seq"] = loss.value("last_seq", Json());
    (*target)["last_timestamp"] = loss.value("last_timestamp", Json());
  }
  (*target)["count"] = old + count;
}

bool WritePage(TIFF* file, const Frame& f, const Json& accounting,
               const Json& metadata, int orientation) {
  const auto& h = f.header;
  if (!f.data || h.width <= 0 || h.height <= 0 ||
      f.data->size() != h.PayloadBytes())
    return false;
  const int bytes = static_cast<int>(PixelTypeSize(h.pixel_type));
  if (!TIFFSetField(file, TIFFTAG_IMAGEWIDTH,
                    static_cast<std::uint32_t>(h.width)) ||
      !TIFFSetField(file, TIFFTAG_IMAGELENGTH,
                    static_cast<std::uint32_t>(h.height)) ||
      !TIFFSetField(file, TIFFTAG_BITSPERSAMPLE, 8 * bytes) ||
      !TIFFSetField(file, TIFFTAG_SAMPLESPERPIXEL, 1) ||
      !TIFFSetField(file, TIFFTAG_SAMPLEFORMAT,
                    h.pixel_type == PixelType::kUint16 ? SAMPLEFORMAT_UINT
                    : h.pixel_type == PixelType::kInt32
                        ? SAMPLEFORMAT_INT
                        : SAMPLEFORMAT_IEEEFP) ||
      !TIFFSetField(file, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK) ||
      !TIFFSetField(file, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) ||
      !TIFFSetField(file, TIFFTAG_COMPRESSION, COMPRESSION_NONE) ||
      !TIFFSetField(file, TIFFTAG_ROWSPERSTRIP, h.height) ||
      !TIFFSetField(file, TIFFTAG_SOFTWARE, "AndorEMCCDcontroller stream") ||
      !TIFFSetField(file, TIFFTAG_ORIENTATION, orientation))
    return false;
  Json description{
      {"frame", h}, {"delivery", f.delivery}, {"recording", accounting}};
  if (!metadata.is_null()) description["metadata"] = metadata;
  const auto text = description.dump();
  if (!TIFFSetField(file, TIFFTAG_IMAGEDESCRIPTION, text.c_str())) return false;
  const auto size = static_cast<tmsize_t>(f.data->size());
  return TIFFWriteEncodedStrip(file, 0,
                               const_cast<std::uint8_t*>(f.data->data()),
                               size) == size &&
         TIFFWriteDirectory(file) && TIFFFlush(file);
}
}  // namespace

StreamRecorder::~StreamRecorder() { Stop(); }

bool StreamRecorder::Start(const std::string& path, double max_fps,
                           const Json& metadata, std::size_t queue_frames,
                           std::size_t queue_bytes, std::string* error) {
  Stop();
  if (!std::isfinite(max_fps) || max_fps < 0 || !queue_frames || !queue_bytes) {
    *error = "invalid FPS or queue capacity";
    return false;
  }
#ifdef _WIN32
  const int fd =
      _wopen(std::filesystem::u8path(path).c_str(),
             _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
  const int fd = open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
#endif
  if (fd < 0) {
    *error = "cannot create " + path + ": " + std::strerror(errno);
    return false;
  }
#ifdef _WIN32
  std::FILE* stream = _fdopen(fd, "w+b");
#else
  std::FILE* stream = fdopen(fd, "w+b");
#endif
  if (!stream) {
#ifdef _WIN32
    _close(fd);
#else
    close(fd);
#endif
    *error = "cannot initialize file stream: " + path;
    return false;
  }
  // Flush each completed page through to the OS, including its directory.
  std::setvbuf(stream, nullptr, _IONBF, 0);
  TIFF* file = TIFFClientOpen(path.c_str(), "w8", stream, ReadFile, WriteFile,
                              SeekFile, KeepFileOpen, FileSize, NoMap, NoUnmap);
  if (!file) {
    std::fclose(stream);
    *error = "cannot initialize BigTIFF: " + path;
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = {};
    status_.active = true;
    stopping_ = false;
    have_last_ = false;
    max_fps_ = max_fps;
    max_frames_ = queue_frames;
    max_bytes_ = queue_bytes;
    bytes_ = 0;
    metadata_ = metadata;
    tiff_orientation_ = TiffOrientation(OrientationFromMetadata(metadata));
    pending_loss_ = {{"server_loss", {{"count", 0}}},
                     {"client_loss", {{"count", 0}}}};
  }
  try {
    writer_ = std::thread(&StreamRecorder::WriteLoop, this, file);
  } catch (const std::exception& e) {
    TIFFClose(file);
    std::fclose(stream);
    std::lock_guard<std::mutex> lock(mutex_);
    status_.active = false;
    status_.error = e.what();
    *error = e.what();
    return false;
  }
  return true;
}

void StreamRecorder::Submit(const Frame& frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!status_.active || stopping_) return;
  for (const char* key : {"server_loss", "client_loss"}) {
    if (frame.delivery.contains(key))
      MergeLoss(&pending_loss_[key], frame.delivery.at(key));
  }
  const double timestamp = frame.header.timestamp;
  if (!std::isfinite(timestamp)) {
    ++status_.failed;
    status_.error = "non-finite frame timestamp";
    return;
  }
  const bool same_run = have_last_ && last_run_ == frame.header.run_id;
  if (max_fps_ > 0 && same_run && timestamp >= last_timestamp_ &&
      timestamp - last_timestamp_ + 1e-6 < 1.0 / max_fps_) {
    ++status_.rate_skipped;
    return;
  }
  const std::size_t bytes = frame.data ? frame.data->size() : 0;
  if (queue_.size() >= max_frames_ || bytes > max_bytes_ - bytes_) {
    ++status_.queue_dropped;
    return;
  }
  Json accounting{{"schema", "aec_stream_v1"},
                  {"max_fps", max_fps_},
                  {"rate_skipped_total", status_.rate_skipped},
                  {"queue_dropped_total", status_.queue_dropped},
                  {"loss_since_previous_saved", pending_loss_}};
  queue_.push_back({frame, std::move(accounting)});
  pending_loss_ = {{"server_loss", {{"count", 0}}},
                   {"client_loss", {{"count", 0}}}};
  bytes_ += bytes;
  status_.queued = queue_.size();
  last_timestamp_ = timestamp;
  last_run_ = frame.header.run_id;
  have_last_ = true;
  ready_.notify_one();
}

void StreamRecorder::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    status_.active = false;
  }
  ready_.notify_one();
  if (writer_.joinable()) writer_.join();
}

StreamRecordingStatus StreamRecorder::Status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

void StreamRecorder::WriteLoop(void* handle) {
  auto* file = static_cast<TIFF*>(handle);
  try {
    bool first = true;
    for (;;) {
      Pending pending;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (queue_.empty()) break;
        pending = std::move(queue_.front());
        queue_.pop_front();
        bytes_ -= pending.frame.data ? pending.frame.data->size() : 0;
        status_.queued = queue_.size();
      }
      if (!WritePage(file, pending.frame, pending.accounting,
                     first ? metadata_ : Json(), tiff_orientation_)) {
        throw std::runtime_error(
            "BigTIFF write/flush failed (check disk space)");
      }
      first = false;
      std::lock_guard<std::mutex> lock(mutex_);
      ++status_.saved;
    }
  } catch (const std::exception& e) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.error = e.what();
    status_.active = false;
    status_.failed += 1 + queue_.size();
    queue_.clear();
    bytes_ = 0;
    status_.queued = 0;
  }
  auto* stream = static_cast<std::FILE*>(TIFFClientdata(file));
  TIFFClose(file);
  if (std::fclose(stream) != 0) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.error = "failed to close recording file";
  }
}
}  // namespace AndorEMCCDcontroller
