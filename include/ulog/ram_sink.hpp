#pragma once

#include <vector>

#include "ulog/sink.hpp"

namespace ulog {

// In-memory sink, mainly for unit tests and small logs.
class RamSink final : public Sink {
 public:
  bool write(const uint8_t* data, size_t size) override {
    data_.insert(data_.end(), data, data + size);
    return true;
  }

  const std::vector<uint8_t>& bytes() const { return data_; }
  void clear() { data_.clear(); }

 private:
  std::vector<uint8_t> data_;
};

}  // namespace ulog
