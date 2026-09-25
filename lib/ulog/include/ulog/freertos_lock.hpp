#pragma once

// FreeRTOS implementation of WriterLock (ESP32 builds).

#if !defined(ESP_PLATFORM)
#error "ulog/freertos_lock.hpp requires an ESP32 (FreeRTOS) build"
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

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

}  // namespace ulog
