#pragma once

// Flight-recorder facade over the ulog library (ESP32 + Arduino).
//
// One ULog stream per boot, spooled in RAM (SpoolSink: no allocation in
// flight, no flash access while flying), appended to a single file on
// LittleFS at each persist() — e.g. once landed:
//
//   class MyLogger : public ulog::LoggerBase {
//    protected:
//     bool on_declare() override { ... declare() ...; }
//    public:
//     void log_imu(...) { imu_.log(...); }   // called from any task
//   };
//
//   MyLogger logger{"/log%03d.ulg"};   // one file per boot, %03d pattern
//   logger.begin();                     // reserves the spool, runs on_declare()
//   ... logger.log_imu(...) ...         // RAM only, any task, never blocks
//   logger.persist();                   // landed: append everything to flash
//   logger.end();                       // persist + close
//
// When the spool is full, whole messages are dropped and the lost time is
// reported as ULog dropouts ('O'); a crash only loses the tail since the
// last persist(). The path may contain a printf-style integer pattern:
// each boot takes the first free number, so logs accumulate across boots.

#if !defined(ESP_PLATFORM)
#error "ulog/logger.hpp requires an ESP32 (Arduino + LittleFS) build"
#endif

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ulog/freertos_lock.hpp"
#include "ulog/log_summary.hpp"
#include "ulog/littlefs_sink.hpp"
#include "ulog/spool_sink.hpp"
#include "ulog/writer.hpp"

namespace ulog {

class LoggerBase {
 public:
  struct Config {
    size_t spool_capacity = 2 * 1024 * 1024;  // RAM between two persist()
  };

  explicit LoggerBase(const char* path) : LoggerBase(path, Config()) {}
  LoggerBase(const char* path, const Config& config)
      : path_(path), config_(config), file_(active_path_) {}

  virtual ~LoggerBase() {
    end();
    delete spool_;
    spool_ = nullptr;
    free(buffer_a_);
    free(buffer_b_);
  }

  LoggerBase(const LoggerBase&) = delete;
  LoggerBase& operator=(const LoggerBase&) = delete;

  // Reserves the spool and starts a fresh stream. False means a boot error:
  // no RAM for the spool, or the derived class failed to declare.
  bool begin() {
    if (writer_ != nullptr) return true;
    if (!allocate_spool()) return false;

    writer_ = new Writer(*spool_, Writer::Config(), &lock_);
    if (!on_declare()) {
      delete writer_;
      writer_ = nullptr;
      return false;
    }
    return true;
  }

  // Appends everything spooled so far to the file. Returns the number of
  // bytes persisted; 0 on failure or if the session is not running.
  size_t persist() {
    if (writer_ == nullptr) return 0;
    if (!file_open_ && !open_file()) return 0;

    lock_.lock();
    size_t size = 0;
    const uint8_t* bytes = spool_->rotate(&size);
    const uint32_t dropped = spool_->take_dropped_ms();
    lock_.unlock();

    if (size == 0) return 0;
    if (!file_.write(bytes, size)) return 0;  // spool full or flash error
    file_.sync();
    persisted_bytes_ += size;

    if (dropped > 0) {
      dropped_total_ms_ += dropped;
      writer_->log_dropout(dropped);  // reported in the next persist()
    }
    return size;
  }

  // persist() + file close. The stream is over; logging returns false.
  void end() {
    if (writer_ == nullptr) return;
    persist();
    if (file_open_) {
      file_.sync();
      file_.close();
      file_open_ = false;
    }
    delete writer_;
    writer_ = nullptr;
  }

  // Formats the whole LittleFS partition: every log file is gone. Takes
  // seconds (full erase), ground only. The current stream is unaffected.
  bool erase_flash() {
    if (file_open_) {
      file_.close();
      file_open_ = false;
    }
    return LittleFS.format();
  }

  // Drops everything not persisted and starts a fresh stream: on_declare()
  // runs again (message handles of the derived class are refreshed) and the
  // next persist() opens a new file.
  bool restart() {
    if (writer_ == nullptr) return begin();
    delete writer_;
    writer_ = nullptr;

    if (file_open_) {
      file_.close();
      file_open_ = false;
    }
    lock_.lock();
    size_t size = 0;
    spool_->rotate(&size);  // discard everything not persisted
    lock_.unlock();

    writer_ = new Writer(*spool_, Writer::Config(), &lock_);
    if (!on_declare()) {
      delete writer_;
      writer_ = nullptr;
      return false;
    }
    return true;
  }

  // Prints a human summary of the log on `out` (e.g. Serial): topics, sample
  // counts, texts, params, dropouts. Walks the file persisted so far plus
  // the RAM spool not yet persisted: the whole log of this boot.
  void debug_dump(Print& out) {
    UlogSummary walker(emit_line, &out);

    if (persisted_bytes_ > 0 && active_path_[0] != '\0') {
      if (file_open_) {
        file_.sync();
      }
      fs::File file = LittleFS.open(active_path_, "r");
      if (file) {
        uint8_t chunk[512];
        size_t to_skip = 16;  // ULog file header, not a message
        size_t n;
        while ((n = file.read(chunk, sizeof(chunk))) > 0) {
          size_t offset = 0;
          if (to_skip > 0) {
            const size_t skip = to_skip < n ? to_skip : n;
            to_skip -= skip;
            offset = skip;
          }
          walker.feed(chunk + offset, n - offset);
        }
        file.close();
      }
    }

    size_t spool_size = 0;
    if (spool_ != nullptr) {
      lock_.lock();
      const uint8_t* bytes = spool_->peek(&spool_size);
      if (spool_size > 0) {
        walker.feed(bytes, spool_size);
      }
      lock_.unlock();
    }

    const LogSummary summary = walker.finish();

    out.println("=== ulog debug dump ===");
    out.printf("file: %s (%u bytes persisted this boot)\n",
               active_path_[0] != '\0' ? active_path_ : "(not created yet)",
               static_cast<unsigned>(persisted_bytes_));
    out.printf("spool: %u/%u bytes, %u ms dropped, %s\n",
               static_cast<unsigned>(spool_size),
               static_cast<unsigned>(spool_ != nullptr ? spool_->capacity() : 0),
               static_cast<unsigned>(dropped_total_ms_),
               running() ? "running" : "stopped");
    out.printf("stream: %u formats, %u topics, %u samples, %u texts, "
               "%u infos, %u params%s\n",
               summary.formats, summary.subscriptions, summary.samples,
               summary.texts, summary.infos, summary.params,
               summary.truncated ? ", TRUNCATED" : "");
  }

  bool running() const { return writer_ != nullptr; }
  size_t persisted_bytes() const { return persisted_bytes_; }
  uint32_t dropped_ms() const { return dropped_total_ms_; }
  const char* session_path() const { return active_path_; }

  // Text logging, callable from any task at any time: thread-safe, and
  // returns false when no session is running (before begin(), after end()).
  bool log_text(Level level, const char* text) {
    return writer_ != nullptr && writer_->log_text(level, text);
  }
  bool log_text(Level level, const char* text, size_t length) {
    return writer_ != nullptr && writer_->log_text(level, text, length);
  }
  bool log_info(const char* text) { return log_text(Level::Info, text); }
  bool log_warning(const char* text) { return log_text(Level::Warning, text); }
  bool log_error(const char* text) { return log_text(Level::Error, text); }

  size_t pending() const {
    lock_.lock();
    const size_t value = spool_ != nullptr ? spool_->pending() : 0;
    lock_.unlock();
    return value;
  }

  // >= 80% of the spool capacity: persist() early if a stall is acceptable.
  bool spool_near_full() const {
    if (spool_ == nullptr) return false;
    lock_.lock();
    const bool full = spool_->near_full(spool_->capacity() * 4 / 5);
    lock_.unlock();
    return full;
  }

 protected:
  // The derived class declares its messages here, after each begin() and
  // restart(). Return false (and stop) if a declare() fails.
  virtual bool on_declare() = 0;

  Message declare(const char* name, const char* fields) {
    return writer_ != nullptr ? writer_->declare(name, fields) : Message();
  }
  bool add_info(const char* name, const char* value) {
    return writer_ != nullptr && writer_->add_info(name, value);
  }
  template <typename T>
  bool add_info(const char* name, T value) {
    return writer_ != nullptr && writer_->add_info(name, value);
  }
  bool add_param(const char* name, int32_t value) {
    return writer_ != nullptr && writer_->add_param(name, value);
  }
  bool add_param(const char* name, float value) {
    return writer_ != nullptr && writer_->add_param(name, value);
  }

 private:
  static void emit_line(void* ctx, const char* line) {
    static_cast<Print*>(ctx)->println(line);
  }

  bool allocate_spool() {
    if (spool_ != nullptr) return true;
    buffer_a_ = static_cast<uint8_t*>(
        heap_caps_malloc(config_.spool_capacity, MALLOC_CAP_SPIRAM));
    if (buffer_a_ == nullptr) {
      buffer_a_ = static_cast<uint8_t*>(malloc(config_.spool_capacity));
    }
    buffer_b_ = static_cast<uint8_t*>(
        heap_caps_malloc(config_.spool_capacity, MALLOC_CAP_SPIRAM));
    if (buffer_b_ == nullptr) {
      buffer_b_ = static_cast<uint8_t*>(malloc(config_.spool_capacity));
    }
    if (buffer_a_ == nullptr || buffer_b_ == nullptr) {
      free(buffer_a_);
      free(buffer_b_);
      buffer_a_ = buffer_b_ = nullptr;
      return false;
    }
    spool_ = new SpoolSink(buffer_a_, buffer_b_, config_.spool_capacity,
                           default_clock_us);
    return true;
  }

  bool open_file() {
    if (!LittleFS.begin(true)) return false;
    if (strchr(path_, '%') != nullptr) {
      bool found = false;
      for (int i = 0; i < 1000; ++i) {
        snprintf(active_path_, sizeof(active_path_), path_, i);
        if (!LittleFS.exists(active_path_)) {
          found = true;
          break;
        }
      }
      if (!found) return false;
    } else {
      snprintf(active_path_, sizeof(active_path_), "%s", path_);
    }
    // first open of this stream truncates, later opens append
    file_open_ = file_.open(persisted_bytes_ > 0);
    return file_open_;
  }

  const char* path_;
  Config config_;
  mutable FreeRtosLock lock_;
  SpoolSink* spool_ = nullptr;
  Writer* writer_ = nullptr;
  uint8_t* buffer_a_ = nullptr;
  uint8_t* buffer_b_ = nullptr;
  LittleFsSink file_;
  bool file_open_ = false;
  char active_path_[64] = {};
  size_t persisted_bytes_ = 0;
  uint32_t dropped_total_ms_ = 0;
};

}  // namespace ulog
