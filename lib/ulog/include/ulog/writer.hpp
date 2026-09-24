#pragma once

#include <stddef.h>
#include <stdint.h>
#include <type_traits>

#include "ulog/sink.hpp"

namespace ulog {

enum class Level : uint8_t {
  Emergency = 0,
  Alert = 1,
  Critical = 2,
  Error = 3,
  Warning = 4,
  Notice = 5,
  Info = 6,
  Debug = 7,
};

// Clock used when the Writer config does not provide one.
// esp_timer_get_time() on ESP32, steady_clock elsewhere.
uint64_t default_clock_us();

// Type names as spelled in the ULog format, used for 'I' and 'P' keys.
inline const char* type_name_of(int8_t) { return "int8_t"; }
inline const char* type_name_of(uint8_t) { return "uint8_t"; }
inline const char* type_name_of(int16_t) { return "int16_t"; }
inline const char* type_name_of(uint16_t) { return "uint16_t"; }
inline const char* type_name_of(int32_t) { return "int32_t"; }
inline const char* type_name_of(uint32_t) { return "uint32_t"; }
inline const char* type_name_of(int64_t) { return "int64_t"; }
inline const char* type_name_of(uint64_t) { return "uint64_t"; }
inline const char* type_name_of(float) { return "float"; }
inline const char* type_name_of(double) { return "double"; }
inline const char* type_name_of(bool) { return "bool"; }

// Destination of encoded bytes: a Sink directly, or a ring buffer when the
// writes are drained asynchronously. A message written between start_message()
// and finish_message() is either fully stored or fully discarded.
class WriterTarget {
 public:
  virtual ~WriterTarget() = default;
  virtual bool write(const uint8_t* data, size_t size) = 0;
  virtual void start_message() {}
  virtual bool finish_message(bool ok) { return ok; }
};

// Serializes Writer calls when several tasks log concurrently.
class WriterLock {
 public:
  virtual ~WriterLock() = default;
  virtual void lock() = 0;
  virtual void unlock() = 0;
};

class DirectWriterTarget final : public WriterTarget {
 public:
  DirectWriterTarget() = default;
  explicit DirectWriterTarget(Sink& sink) : sink_(&sink) {}

  bool write(const uint8_t* data, size_t size) override {
    return sink_ != nullptr && sink_->write(data, size);
  }

 private:
  Sink* sink_ = nullptr;
};

class Writer;

// Handle on a declared message format. Copyable, cheap.
class Message {
 public:
  Message() = default;

  explicit operator bool() const;

  // Size of the payload to pass to log(), trailing _padding fields excluded.
  uint16_t payload_size() const;

  bool log(const void* payload);

  template <typename T>
  bool log(const T& payload) {
    static_assert(std::is_trivially_copyable<T>::value,
                  "ulog payloads must be trivially copyable");
    return sizeof(T) == logged_size_ && log(static_cast<const void*>(&payload));
  }

 private:
  friend class Writer;
  Message(Writer& writer, uint8_t index, uint16_t logged_size);

  Writer* writer_ = nullptr;
  uint8_t index_ = 0;
  uint16_t logged_size_ = 0;
};

// Encodes a ULog stream (header, definitions, data) into a WriterTarget.
// Thread-safe when a WriterLock is provided, otherwise single-task only.
class Writer {
 public:
  struct Config {
    uint64_t (*now_us)() = default_clock_us;
    size_t sync_interval_bytes = 4096;  // 0 disables sync messages
  };

  Writer(Sink& sink);
  Writer(Sink& sink, const Config& config);
  Writer(WriterTarget& target, const Config& config, WriterLock* lock);

  // Registers a message format (writes the 'F' message). Must be called
  // before the data section starts. `fields` uses the ULog syntax,
  // e.g. "uint64_t timestamp;float[3] gyro;".
  Message declare(const char* name, const char* fields);

  // 'I' message: string value, encoded as "char[len] name".
  bool add_info(const char* name, const char* value);
  // 'I' message: numeric value, encoded as "<type> name".
  template <typename T>
  typename std::enable_if<std::is_arithmetic<T>::value, bool>::type add_info(
      const char* name, T value) {
    return write_key_value('I', type_name_of(value), name, &value, sizeof(T));
  }

  // 'P' messages, restricted to int32_t and float per the ULog spec.
  bool add_param(const char* name, int32_t value) {
    return write_key_value('P', "int32_t", name, &value, sizeof(value));
  }
  bool add_param(const char* name, float value) {
    return write_key_value('P', "float", name, &value, sizeof(value));
  }

  // 'L' message: printf-style text with level.
  bool log_text(Level level, const char* text);
  bool log_text(Level level, const char* text, size_t length);

  // 'O' message: duration in ms of lost logging time, e.g. a buffer overflow.
  bool log_dropout(uint32_t duration_ms);

  size_t bytes_written() const { return bytes_; }

 private:
  friend class Message;
  friend struct DeclareCtx;

  static constexpr size_t kMaxFormats = 32;
  static constexpr size_t kMaxNameLen = 23;
  static constexpr uint16_t kNoTimestamp = 0xFFFF;

  struct Format {
    char name[kMaxNameLen + 1] = {};
    uint16_t full_size = 0;    // including trailing padding
    uint16_t logged_size = 0;  // trailing _padding fields excluded
    uint16_t ts_offset = kNoTimestamp;
    uint16_t msg_id = 0;
    bool subscribed = false;
    uint64_t last_timestamp = 0;
  };

  bool write_payload(uint8_t index, const void* payload);

  bool begin_message(uint8_t type, size_t payload_size);
  bool part(const void* data, size_t size);
  void begin_frame();
  bool end_frame(bool ok);
  bool write_key_value(uint8_t type, const char* type_str, const char* name,
                       const void* value, size_t value_size);
  void maybe_sync();
  uint16_t nested_type_size(const char* type, size_t len);
  void write_file_header();

  DirectWriterTarget direct_target_;
  WriterTarget* target_;
  WriterLock* lock_;
  uint64_t (*now_us_)();
  size_t sync_interval_;
  size_t bytes_ = 0;
  size_t last_sync_ = 0;
  size_t msg_start_bytes_ = 0;
  bool data_started_ = false;
  uint16_t next_msg_id_ = 0;
  uint16_t format_count_ = 0;
  Format formats_[kMaxFormats];
};

inline Message::operator bool() const { return writer_ != nullptr; }

inline uint16_t Message::payload_size() const { return logged_size_; }

inline bool Message::log(const void* payload) {
  return writer_ != nullptr && writer_->write_payload(index_, payload);
}

inline Message::Message(Writer& writer, uint8_t index, uint16_t logged_size)
    : writer_(&writer), index_(index), logged_size_(logged_size) {}

}  // namespace ulog
