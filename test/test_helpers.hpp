#pragma once

#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#include "ulog/ram_sink.hpp"
#include "ulog/writer.hpp"

namespace ulog_test {

inline uint64_t g_now = 1000;

inline uint64_t test_now() { return g_now; }

inline ulog::Writer::Config test_config() {
  ulog::Writer::Config config;
  config.now_us = test_now;
  return config;
}

struct __attribute__((packed)) ImuSample {
  uint64_t timestamp;
  float gyro[3];
  float accel[3];
};

struct __attribute__((packed)) BaroSample {
  uint64_t timestamp;
  float pressure_pa;
  float temperature_deg;
};

struct Msg {
  uint8_t type = 0;
  std::vector<uint8_t> payload;
};

class Reader {
 public:
  explicit Reader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}

  bool next(Msg& msg) {
    if (pos_ + 3 > bytes_.size()) return false;
    size_t len = bytes_[pos_] | (bytes_[pos_ + 1] << 8);
    msg.type = bytes_[pos_ + 2];
    pos_ += 3;
    if (pos_ + len > bytes_.size()) return false;
    msg.payload.assign(bytes_.begin() + pos_, bytes_.begin() + pos_ + len);
    pos_ += len;
    return true;
  }

 private:
  const std::vector<uint8_t>& bytes_;
  size_t pos_ = 16;  // skip file header
};

inline std::string as_str(const uint8_t* data, size_t len) {
  return std::string(reinterpret_cast<const char*>(data), len);
}

inline std::string payload_str(const Msg& msg) {
  return as_str(msg.payload.data(), msg.payload.size());
}

inline std::string sub_str(const Msg& msg, size_t from, size_t to) {
  return as_str(msg.payload.data() + from, to - from);
}

inline uint16_t le16(const uint8_t* p) { return p[0] | (p[1] << 8); }

inline uint32_t le32(const uint8_t* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t le64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

// Sink that always fails, to exercise error paths.
class FailingSink final : public ulog::Sink {
 public:
  bool write(const uint8_t*, size_t) override { return false; }
};

// Deterministic, half-repetitive test pattern: compressible but not trivially so.
inline std::vector<uint8_t> make_pattern(size_t size) {
  std::vector<uint8_t> pattern(size);
  uint32_t x = 12345;
  for (size_t i = 0; i < size; ++i) {
    x = x * 1664525u + 1013904223u;
    pattern[i] = (i % 64 < 48) ? static_cast<uint8_t>(i)
                               : static_cast<uint8_t>(x >> 24);
  }
  return pattern;
}

}  // namespace ulog_test
