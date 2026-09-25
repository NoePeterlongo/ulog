#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ulog/writer.hpp"

namespace ulog {

// RAM spool for whole messages, backed by two caller-provided buffers of
// `capacity` bytes each. Nothing is allocated after construction, so a full
// spool degrades to message-atomic drops with dropout accounting instead of
// an allocation failure. All methods must be serialized externally (share
// the Writer's WriterLock).
//
// rotate() hands the spooled bytes to a slow consumer (e.g. flash) in O(1)
// while logging continues into the second buffer.
class SpoolSink final : public WriterTarget {
 public:
  SpoolSink(uint8_t* buffer_a, uint8_t* buffer_b, size_t capacity,
            uint64_t (*now_us)())
      : buffers_{buffer_a, buffer_b}, capacity_(capacity), now_us_(now_us) {}

  SpoolSink(const SpoolSink&) = delete;
  SpoolSink& operator=(const SpoolSink&) = delete;

  void start_message() override { msg_start_ = size_; }

  bool write(const uint8_t* data, size_t size) override {
    if (size_ + size > capacity_) return false;
    memcpy(active() + size_, data, size);
    size_ += size;
    return true;
  }

  bool finish_message(bool ok) override {
    if (!ok) {
      size_ = msg_start_;
      if (drop_start_us_ == 0) drop_start_us_ = now_us_();
      return false;
    }
    if (drop_start_us_ != 0) {
      dropped_ms_ += static_cast<uint32_t>((now_us_() - drop_start_us_) / 1000);
      drop_start_us_ = 0;
    }
    return true;
  }

  // Hands out everything spooled so far and makes the second buffer active.
  // The returned pointer stays valid until the next rotate().
  const uint8_t* rotate(size_t* out_size) {
    *out_size = size_;
    active_index_ ^= 1;
    size_ = 0;
    return buffers_[active_index_ ^ 1];
  }

  // Read-only view of the bytes spooled so far, for debug output.
  const uint8_t* peek(size_t* out_size) const {
    *out_size = size_;
    return buffers_[active_index_];
  }

  // Dropout time in ms accumulated since the previous call. A drop window
  // still open (spool full right now) closes with the next successful write.
  uint32_t take_dropped_ms() {
    const uint32_t value = dropped_ms_;
    dropped_ms_ = 0;
    return value;
  }

  size_t pending() const { return size_; }
  size_t capacity() const { return capacity_; }
  bool near_full(size_t watermark) const { return size_ >= watermark; }

 private:
  uint8_t* active() { return buffers_[active_index_]; }

  uint8_t* buffers_[2];
  size_t capacity_;
  uint64_t (*now_us_)();
  size_t size_ = 0;
  size_t msg_start_ = 0;
  uint32_t dropped_ms_ = 0;
  uint64_t drop_start_us_ = 0;
  uint8_t active_index_ = 0;
};

}  // namespace ulog
