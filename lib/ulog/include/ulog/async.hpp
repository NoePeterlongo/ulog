#pragma once

// AsyncWriter: ESP32/FreeRTOS writer task draining a ring buffer into a Sink.
// Producers log through the same Writer API without ever blocking; on
// overflow whole messages are dropped and reported as ULog dropout ('O')
// messages. Stop producer tasks before calling stop().

#if !defined(ESP_PLATFORM)
#error "ulog/async.hpp requires an ESP32 (FreeRTOS) build"
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <stdlib.h>

#include "ulog/ring.hpp"
#include "ulog/writer.hpp"

namespace ulog {

class FreeRtosLock final : public WriterLock {
 public:
  FreeRtosLock() : mutex_(xSemaphoreCreateMutex()) {}
  ~FreeRtosLock() override {
    if (mutex_ != nullptr) vSemaphoreDelete(mutex_);
  }
  FreeRtosLock(const FreeRtosLock&) = delete;
  FreeRtosLock& operator=(const FreeRtosLock&) = delete;

  void lock() override { xSemaphoreTake(mutex_, portMAX_DELAY); }
  void unlock() override { xSemaphoreGive(mutex_); }

 private:
  SemaphoreHandle_t mutex_;
};

class AsyncWriter {
 public:
  struct Config {
    uint8_t* ring_storage = nullptr;
    size_t ring_size = 0;
    size_t drain_chunk = 1024;
    // Max time (ms) data may sit in the ring before being written out.
    // Larger values batch sink writes (fewer flash operations on LittleFS);
    // 0 writes and flushes as soon as data is available.
    uint32_t flush_interval_ms = 100;
    uint32_t task_stack_bytes = 4096;
    UBaseType_t task_priority = 3;
    // 0 or 1 to pin the drain task to a core, tskNO_AFFINITY (default) otherwise.
    BaseType_t task_core = tskNO_AFFINITY;
  };

  AsyncWriter(Sink& sink, const Writer::Config& writer_config,
              const Config& config)
      : sink_(sink),
        ring_(config.ring_storage, config.ring_size),
        target_(ring_, writer_config.now_us),
        writer_(target_, writer_config, &lock_),
        config_(config) {}

  ~AsyncWriter() { stop(); }
  AsyncWriter(const AsyncWriter&) = delete;
  AsyncWriter& operator=(const AsyncWriter&) = delete;

  bool start() {
    if (task_ != nullptr) return true;
    if (ring_.capacity() == 0 || config_.drain_chunk == 0) return false;
    chunk_ = static_cast<uint8_t*>(malloc(config_.drain_chunk));
    done_ = xSemaphoreCreateBinary();
    const BaseType_t created =
        (config_.task_core == tskNO_AFFINITY)
            ? xTaskCreate(drain_entry, "ulog_drain", config_.task_stack_bytes,
                          this, config_.task_priority, &task_)
            : xTaskCreatePinnedToCore(drain_entry, "ulog_drain",
                                     config_.task_stack_bytes, this,
                                     config_.task_priority, &task_,
                                     config_.task_core);
    if (chunk_ == nullptr || done_ == nullptr || created != pdPASS) {
      free(chunk_);
      chunk_ = nullptr;
      if (done_ != nullptr) {
        vSemaphoreDelete(done_);
        done_ = nullptr;
      }
      task_ = nullptr;
      return false;
    }
    return true;
  }

  // Flushes everything staged so far and joins the drain task.
  // Producers must be stopped first.
  void stop() {
    if (task_ == nullptr) return;
    stop_requested_ = true;
    xSemaphoreTake(done_, portMAX_DELAY);
    task_ = nullptr;
    vSemaphoreDelete(done_);
    done_ = nullptr;
    free(chunk_);
    chunk_ = nullptr;
  }

  Writer& writer() { return writer_; }

  Message declare(const char* name, const char* fields) {
    return writer_.declare(name, fields);
  }
  bool add_info(const char* name, const char* value) {
    return writer_.add_info(name, value);
  }
  template <typename T>
  bool add_info(const char* name, T value) {
    return writer_.add_info(name, value);
  }
  bool add_param(const char* name, int32_t value) {
    return writer_.add_param(name, value);
  }
  bool add_param(const char* name, float value) {
    return writer_.add_param(name, value);
  }
  bool log_text(Level level, const char* text) {
    return writer_.log_text(level, text);
  }
  bool log_text(Level level, const char* text, size_t length) {
    return writer_.log_text(level, text, length);
  }

  size_t ring_used() const { return ring_.used(); }

 private:
  static void drain_entry(void* ctx) { static_cast<AsyncWriter*>(ctx)->drain(); }

  void drain() {
    const TickType_t interval = pdMS_TO_TICKS(config_.flush_interval_ms);
    TickType_t last_write = xTaskGetTickCount();
    bool dirty = false;
    while (true) {
      const TickType_t now = xTaskGetTickCount();
      const bool due = interval == 0 || now - last_write >= interval;
      if (ring_.used() > 0 &&
          (ring_.used() >= config_.drain_chunk || due || stop_requested_)) {
        const size_t n = ring_.read(chunk_, config_.drain_chunk);
        sink_.write(chunk_, n);
        dirty = true;
        last_write = now;
        continue;
      }

      const uint32_t dropped = target_.take_dropped_ms();
      if (dropped > 0) writer_.log_dropout(dropped);

      if (stop_requested_ && ring_.used() == 0) break;

      if (dirty) {
        sink_.sync();
        dirty = false;
      }
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    sink_.sync();
    xSemaphoreGive(done_);
    vTaskDelete(nullptr);
  }

  Sink& sink_;
  RingBuffer ring_;
  RingWriterTarget target_;
  FreeRtosLock lock_;
  Writer writer_;
  Config config_;
  uint8_t* chunk_ = nullptr;
  TaskHandle_t task_ = nullptr;
  SemaphoreHandle_t done_ = nullptr;
  volatile bool stop_requested_ = false;
};

}  // namespace ulog
