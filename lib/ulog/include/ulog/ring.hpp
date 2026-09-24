#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <atomic>

#include "ulog/writer.hpp"

namespace ulog {

// SPSC byte ring with message-atomic writes. Producers must be serialized
// (e.g. via a WriterLock); a single consumer drains with read(). A message
// that does not fit entirely is rolled back, so the consumer always reads
// whole messages.
class RingBuffer {
 public:
  RingBuffer(uint8_t* storage, size_t capacity)
      : storage_(storage), capacity_(static_cast<uint32_t>(capacity)) {}

  RingBuffer(const RingBuffer&) = delete;
  RingBuffer& operator=(const RingBuffer&) = delete;

  void start_message() { write_pos_ = head_.load(std::memory_order_relaxed); }

  bool write(const uint8_t* data, size_t size) {
    const uint32_t tail = tail_.load(std::memory_order_acquire);
    if (size > capacity_ - (write_pos_ - tail)) return false;
    const uint32_t index = write_pos_ % capacity_;
    const size_t first = (capacity_ - index < size) ? capacity_ - index : size;
    memcpy(storage_ + index, data, first);
    if (size > first) memcpy(storage_, data + first, size - first);
    write_pos_ += static_cast<uint32_t>(size);
    return true;
  }

  void commit_message() { head_.store(write_pos_, std::memory_order_release); }

  void rollback_message() { write_pos_ = head_.load(std::memory_order_relaxed); }

  size_t read(uint8_t* out, size_t max) {
    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    const uint32_t head = head_.load(std::memory_order_acquire);
    const uint32_t available = head - tail;
    const size_t n = (max < available) ? max : available;
    const uint32_t index = tail % capacity_;
    const size_t first = (capacity_ - index < n) ? capacity_ - index : n;
    memcpy(out, storage_ + index, first);
    if (n > first) memcpy(out + first, storage_, n - first);
    tail_.store(tail + static_cast<uint32_t>(n), std::memory_order_release);
    return n;
  }

  size_t used() const {
    return head_.load(std::memory_order_acquire) -
           tail_.load(std::memory_order_relaxed);
  }

  size_t capacity() const { return capacity_; }

 private:
  uint8_t* storage_;
  uint32_t capacity_;
  std::atomic<uint32_t> head_{0};
  std::atomic<uint32_t> tail_{0};
  uint32_t write_pos_ = 0;
};

// WriterTarget staging whole messages into a RingBuffer. A failed message is
// rolled back and its elapsed time recorded as a dropout.
class RingWriterTarget final : public WriterTarget {
 public:
  RingWriterTarget(RingBuffer& ring, uint64_t (*now_us)())
      : ring_(ring), now_us_(now_us) {}

  void start_message() override { ring_.start_message(); }

  bool write(const uint8_t* data, size_t size) override {
    return ring_.write(data, size);
  }

  bool finish_message(bool ok) override {
    if (!ok) {
      ring_.rollback_message();
      if (drop_start_us_ == 0) drop_start_us_ = now_us_();
      return false;
    }
    ring_.commit_message();
    if (drop_start_us_ != 0) {
      dropped_ms_.fetch_add(
          static_cast<uint32_t>((now_us_() - drop_start_us_) / 1000),
          std::memory_order_relaxed);
      drop_start_us_ = 0;
    }
    return true;
  }

  // Consumer side: total dropout time in ms since the previous call.
  uint32_t take_dropped_ms() {
    return dropped_ms_.exchange(0, std::memory_order_relaxed);
  }

 private:
  RingBuffer& ring_;
  uint64_t (*now_us_)();
  std::atomic<uint32_t> dropped_ms_{0};
  uint64_t drop_start_us_ = 0;
};

}  // namespace ulog
