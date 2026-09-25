// Minimal ulog example: declare one topic, log 1000 samples, done.
//
// Build and run from the repository root:
//   g++ -std=gnu++17 -Wall -Iinclude examples/native_minimal/main.cpp src/writer.cpp src/format.cpp -o /tmp/minimal && /tmp/minimal
//
// Then inspect the log with any ULog tool, e.g.:
//   pip install pyulog && pyulog info /tmp/ulog_minimal.ulg

#include <cstdio>

#include "ulog/file_sink.hpp"
#include "ulog/writer.hpp"

namespace {

struct __attribute__((packed)) ImuSample {
  uint64_t timestamp;
  float gyro_rad[3];
  float accel_mps2[3];
};

uint64_t host_time_us() {
  static uint64_t now = 0;
  now += 5000;  // pretend we sample at 200 Hz
  return now;
}

}  // namespace

int main() {
  ulog::FileSink sink("/tmp/ulog_minimal.ulg");
  if (!sink.is_open()) {
    std::printf("cannot open /tmp/ulog_minimal.ulg\n");
    return 1;
  }

  ulog::Writer::Config config;
  config.now_us = host_time_us;
  ulog::Writer logger{sink, config};

  logger.add_info("sys_name", "ulog-native-minimal");
  auto imu = logger.declare(
      "sensor_imu", "uint64_t timestamp;float[3] gyro_rad;float[3] accel_mps2;");
  if (!static_cast<bool>(imu)) {
    std::printf("declare() failed\n");
    return 1;
  }

  for (int i = 0; i < 1000; ++i) {
    ImuSample sample{host_time_us(), {0.01f * i, 0.f, 0.f}, {9.81f, 0.f, 0.f}};
    imu.log(sample);
  }
  logger.log_text(ulog::Level::Info, "end of example");

  sink.sync();
  sink.close();
  std::printf("wrote /tmp/ulog_minimal.ulg (%u bytes)\n",
              static_cast<unsigned>(logger.bytes_written()));
  return 0;
}
