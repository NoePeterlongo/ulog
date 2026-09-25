#pragma once

#include <cstdio>

#include "ulog/sink.hpp"

namespace ulog {

// Host-side sink writing to a plain file. Useful for host tests and for
// generating reference logs.
class FileSink final : public Sink {
 public:
  explicit FileSink(const char* path) : file_(std::fopen(path, "wb")) {}
  ~FileSink() override { close(); }
  FileSink(const FileSink&) = delete;
  FileSink& operator=(const FileSink&) = delete;

  bool is_open() const { return file_ != nullptr; }

  bool write(const uint8_t* data, size_t size) override {
    return file_ != nullptr && std::fwrite(data, 1, size, file_) == size;
  }

  bool sync() override { return file_ != nullptr && std::fflush(file_) == 0; }

  bool close() {
    if (file_ == nullptr) return true;
    bool ok = std::fclose(file_) == 0;
    file_ = nullptr;
    return ok;
  }

 private:
  std::FILE* file_;
};

}  // namespace ulog
