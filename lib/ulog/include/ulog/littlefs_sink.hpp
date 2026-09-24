#pragma once

// Sink writing to a file on the ESP32 LittleFS partition.

#if !defined(ESP_PLATFORM)
#error "ulog/littlefs_sink.hpp requires an ESP32 build"
#endif

#include <LittleFS.h>

#include "ulog/sink.hpp"

namespace ulog {

class LittleFsSink final : public Sink {
 public:
  // `path` must outlive the sink. `max_bytes` caps the file size, 0 = no cap.
  LittleFsSink(const char* path, size_t max_bytes = 0, bool format_on_fail = true)
      : path_(path), max_bytes_(max_bytes), format_on_fail_(format_on_fail) {}

  LittleFsSink(const LittleFsSink&) = delete;
  LittleFsSink& operator=(const LittleFsSink&) = delete;

  bool open() {
    if (!LittleFS.begin(format_on_fail_)) return false;
    file_ = LittleFS.open(path_, FILE_WRITE);
    return static_cast<bool>(file_);
  }

  bool write(const uint8_t* data, size_t size) override {
    if (!file_ || (max_bytes_ != 0 && file_.size() + size > max_bytes_)) {
      return false;
    }
    return file_.write(data, size) == size;
  }

  bool sync() override {
    if (!file_) return false;
    file_.flush();
    return true;
  }

  bool close() {
    if (!file_) return true;
    file_.close();
    return true;
  }

  size_t size() const { return file_ ? file_.size() : 0; }

 private:
  const char* path_;
  size_t max_bytes_;
  bool format_on_fail_;
  fs::File file_;
};

}  // namespace ulog
