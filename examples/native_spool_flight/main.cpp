// Flight-recorder pattern on the host: the same architecture as
// ulog::LoggerBase on ESP32, built from the portable pieces.
//
//   "flight":  log into a SpoolSink (RAM only, no slow sink involved)
//   "landing": rotate() the spool and append everything to the file
//   repeat, then print a human summary with UlogSummary (what
//   LoggerBase::debug_dump() prints on the serial console).
//
// Build and run from the repository root:
//   g++ -std=gnu++17 -Wall -Iinclude examples/native_spool_flight/main.cpp src/writer.cpp src/format.cpp -o /tmp/spool && /tmp/spool

#include <cstdio>

#include "ulog/file_sink.hpp"
#include "ulog/log_summary.hpp"
#include "ulog/spool_sink.hpp"
#include "ulog/writer.hpp"

namespace {

// spool capacity must cover a full flight between two persist():
// here 25 s * 200 Hz * 25 bytes/message is about 125 KB
uint8_t buffer_a[128 * 1024];
uint8_t buffer_b[128 * 1024];

struct __attribute__((packed)) ImuSample {
  uint64_t timestamp;
  float gyro_rad[3];
};

uint64_t host_time_us() {
  static uint64_t now = 0;
  now += 5000;  // pretend we sample at 200 Hz
  return now;
}

void flight(ulog::Message& imu, uint32_t seconds) {
  for (uint32_t i = 0; i < seconds * 200; ++i) {
    imu.log(ImuSample{host_time_us(), {0.1f, 0.f, 0.f}});
  }
}

void print_line(void*, const char* line) { std::printf("  %s\n", line); }

}  // namespace

int main() {
  ulog::FileSink file("/tmp/ulog_flight.ulg");
  if (!file.is_open()) {
    std::printf("cannot open /tmp/ulog_flight.ulg\n");
    return 1;
  }

  ulog::SpoolSink spool(buffer_a, buffer_b, sizeof(buffer_a), host_time_us);
  ulog::Writer logger{spool, ulog::Writer::Config(), nullptr};  // target: RAM

  auto imu = logger.declare("sensor_imu",
                            "uint64_t timestamp;float[3] gyro_rad;");
  if (!static_cast<bool>(imu)) {
    std::printf("declare() failed\n");
    return 1;
  }

  // --- flight 1, then land and persist ---
  logger.log_text(ulog::Level::Notice, "takeoff 1");
  flight(imu, 15);
  logger.log_text(ulog::Level::Notice, "landing 1");
  size_t chunk = 0;
  const uint8_t* chunk_data = spool.rotate(&chunk);  // split from write(): C++
  file.write(chunk_data, chunk);                     // argument order is
  std::printf("landing 1: persisted %u bytes\n",    // unspecified otherwise
              static_cast<unsigned>(chunk));

  // --- flight 2 into the second buffer, then land and persist ---
  logger.log_text(ulog::Level::Notice, "takeoff 2");
  flight(imu, 10);
  logger.log_text(ulog::Level::Notice, "landing 2");
  chunk_data = spool.rotate(&chunk);
  file.write(chunk_data, chunk);
  std::printf("landing 2: persisted %u bytes\n",
              static_cast<unsigned>(chunk));

  file.sync();
  file.close();

  // --- what LoggerBase::debug_dump() prints on the serial console ---
  std::printf("summary of /tmp/ulog_flight.ulg:\n");
  ulog::UlogSummary summary(print_line, nullptr);
  FILE* in = std::fopen("/tmp/ulog_flight.ulg", "rb");
  uint8_t buf[512];
  size_t n;
  bool first = true;
  while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
    if (first) {  // skip the 16-byte ULog file header
      summary.feed(buf + 16, n - 16);
      first = false;
    } else {
      summary.feed(buf, n);
    }
  }
  std::fclose(in);
  const ulog::LogSummary stats = summary.finish();
  std::printf("  formats: %u, samples: %u, texts: %u, truncated: %s\n",
              stats.formats, stats.samples, stats.texts,
              stats.truncated ? "yes" : "no");
  return 0;
}
