#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ulog/async.hpp"
#include "ulog/ram_sink.hpp"
#include "ulog/writer.hpp"

// Flight-recorder facade over the ulog library.
//
// While a session runs, everything is recorded into RAM: producers from any
// task go through the ring, the drain task drains into a RamSink, and flash
// is never touched (no cache stalls for the rest of the system). Once the
// vehicle is safe, write_to_flash() persists the whole session to LittleFS
// in a single burst, optionally LZ4-compressed.
//
// Lifecycle: begin() -> declare()/log*() from any task -> write_to_flash()
// -> begin() again for the next session. Declare message formats after each
// begin(); handles from a previous session are dead. write_to_flash() must
// only be called once all producer tasks have stopped logging.
//
// The path may contain a printf-style integer pattern (e.g. "/log%03d.ulg.lz4"):
// each session takes the first free number, so re-flying creates a new file.
// Without a pattern, the same file is overwritten every session.
class Logger {
 public:
  struct Config {
    size_t ring_size = 64 * 1024;   // producer ring, PSRAM preferred
    bool compress = true;
    uint32_t task_priority = 3;
    BaseType_t task_core = 0;        // Arduino loop() runs on core 1
  };

  explicit Logger(const char* path);
  Logger(const char* path, const Config& config);
  ~Logger();
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  // Starts a recording session into RAM. Returns false on allocation or
  // task-creation failure.
  bool begin();

  // Stops the session and persists it to LittleFS. True when the complete
  // session is on flash.
  bool write_to_flash();

  ulog::Message declare(const char* name, const char* fields) {
    return async_ != nullptr ? async_->declare(name, fields) : ulog::Message();
  }
  bool add_info(const char* name, const char* value) {
    return async_ != nullptr && async_->add_info(name, value);
  }
  template <typename T>
  bool add_info(const char* name, T value) {
    return async_ != nullptr && async_->add_info(name, value);
  }
  bool add_param(const char* name, int32_t value) {
    return async_ != nullptr && async_->add_param(name, value);
  }
  bool add_param(const char* name, float value) {
    return async_ != nullptr && async_->add_param(name, value);
  }
  bool log_text(ulog::Level level, const char* text) {
    return async_ != nullptr && async_->log_text(level, text);
  }

  bool running() const { return async_ != nullptr; }
  size_t bytes_logged() const { return ram_.bytes().size(); }
  size_t last_flash_bytes() const { return last_flash_bytes_; }
  const char* session_path() const { return active_path_; }

 private:
  bool allocate_ring();
  bool resolve_session_path();

  const char* path_;
  Config config_;
  uint8_t* ring_ = nullptr;
  ulog::RamSink ram_;
  ulog::AsyncWriter* async_ = nullptr;
  size_t last_flash_bytes_ = 0;
  char active_path_[64] = {};
};
