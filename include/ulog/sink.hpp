#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ulog {

// Abstraction of the log medium (ram, littlefs, serial, file, ...).
class Sink {
 public:
  virtual ~Sink() = default;

  virtual bool write(const uint8_t* data, size_t size) = 0;

  // Make written data persistent (e.g. flush). No-op by default.
  virtual bool sync() { return true; }
};

}  // namespace ulog
