#include "logger.hpp"

#include <esp_heap_caps.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ulog/littlefs_sink.hpp"
#include "ulog/lz4_sink.hpp"

Logger::Logger(const char* path) : Logger(path, Config()) {}

Logger::Logger(const char* path, const Config& config)
    : path_(path), config_(config) {}

Logger::~Logger() {
  if (async_ != nullptr) {
    async_->stop();
    delete async_;
  }
  free(ring_);
}

bool Logger::allocate_ring() {
  if (ring_ != nullptr) return true;
  ring_ = static_cast<uint8_t*>(
      heap_caps_malloc(config_.ring_size, MALLOC_CAP_SPIRAM));
  if (ring_ == nullptr) {
    ring_ = static_cast<uint8_t*>(malloc(config_.ring_size));
  }
  return ring_ != nullptr;
}

bool Logger::resolve_session_path() {
  if (strchr(path_, '%') == nullptr) {
    snprintf(active_path_, sizeof(active_path_), "%s", path_);
    return true;
  }
  if (!LittleFS.begin(true)) return false;
  for (int i = 0; i < 1000; ++i) {
    char candidate[64];
    snprintf(candidate, sizeof(candidate), path_, i);
    if (!LittleFS.exists(candidate)) {
      snprintf(active_path_, sizeof(active_path_), "%s", candidate);
      return true;
    }
  }
  return false;
}

bool Logger::begin() {
  if (async_ != nullptr) return true;
  if (!allocate_ring()) return false;
  if (!resolve_session_path()) return false;

  ram_.clear();
  ulog::AsyncWriter::Config async_config;
  async_config.ring_storage = ring_;
  async_config.ring_size = config_.ring_size;
  async_config.task_priority = config_.task_priority;
  async_config.task_core = config_.task_core;
  async_ = new ulog::AsyncWriter(ram_, ulog::Writer::Config(), async_config);
  if (!async_->start()) {
    delete async_;
    async_ = nullptr;
    return false;
  }
  return true;
}

bool Logger::write_to_flash() {
  if (async_ == nullptr) return false;
  async_->stop();
  delete async_;
  async_ = nullptr;

  ulog::LittleFsSink flash(active_path_);
  if (!flash.open()) return false;

  const uint8_t* data = ram_.bytes().data();
  const size_t size = ram_.bytes().size();
  bool ok;
  if (config_.compress) {
    ulog::Lz4Sink sink(flash);
    ok = sink.is_valid() && sink.write(data, size) && sink.finish();
  } else {
    ok = flash.write(data, size);
  }
  last_flash_bytes_ = flash.size();
  return ok && flash.sync() && flash.close();
}
