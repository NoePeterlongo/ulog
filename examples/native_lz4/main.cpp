// Optional compression: wrap any sink in a Lz4Sink and produce a standard
// .lz4 file (the ULog spec has no in-format compression, so the artifact
// is log.ulg.lz4, decompressed on the host before analysis).
//
// Needs the vendored lz4 (lib/lz4/), only for this example: nothing else
// in ulog depends on it.
//
// Build and run from the repository root:
//   g++ -std=gnu++17 -Wall -Iinclude -Ilib/lz4/src examples/native_lz4/main.cpp src/writer.cpp src/format.cpp lib/lz4/src/lz4.c lib/lz4/src/lz4hc.c lib/lz4/src/lz4frame.c lib/lz4/src/xxhash.c -o /tmp/lz4demo && /tmp/lz4demo
//
// Decompress and inspect:
//   lz4 -d /tmp/ulog_compressed.ulg.lz4 /tmp/ulog_compressed.ulg
//   pyulog info /tmp/ulog_compressed.ulg

#include <cstdio>

#include "ulog/file_sink.hpp"
#include "ulog/lz4_sink.hpp"
#include "ulog/writer.hpp"

namespace {

struct __attribute__((packed)) ImuSample {
  uint64_t timestamp;
  float gyro_rad[3];
};

uint64_t host_time_us() {
  static uint64_t now = 0;
  now += 5000;
  return now;
}

}  // namespace

int main() {
  ulog::FileSink file("/tmp/ulog_compressed.ulg.lz4");
  if (!file.is_open()) {
    std::printf("cannot open /tmp/ulog_compressed.ulg.lz4\n");
    return 1;
  }

  ulog::Lz4Sink compressed{file};  // wraps the file: Writer sees a plain sink
  if (!compressed.is_valid()) {
    std::printf("lz4 initialization failed\n");
    return 1;
  }
  ulog::Writer logger{compressed};

  auto imu = logger.declare("sensor_imu",
                            "uint64_t timestamp;float[3] gyro_rad;");
  if (!static_cast<bool>(imu)) return 1;

  for (int i = 0; i < 5000; ++i) {
    // smooth data: compresses well
    imu.log(ImuSample{host_time_us(), {0.01f * i, 0.02f * i, 0.f}});
  }
  logger.log_text(ulog::Level::Info, "compressed example");

  const size_t raw = logger.bytes_written();
  compressed.finish();  // frame footer: required for a valid .lz4 file
  file.close();

  std::printf("wrote /tmp/ulog_compressed.ulg.lz4 (%u raw bytes, "
              "decompress with: lz4 -d)\n",
              static_cast<unsigned>(raw));
  return 0;
}
