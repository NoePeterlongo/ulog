#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <math.h>

#include "ulog/async.hpp"
#include "ulog/littlefs_sink.hpp"
#include "ulog/lz4_sink.hpp"

namespace {

struct __attribute__((packed)) ImuSample {
  uint64_t timestamp;
  float gyro_rad[3];
  float accel_mps2[3];
};

struct __attribute__((packed)) BaroSample {
  uint64_t timestamp;
  float pressure_pa;
  float temperature_deg;
};

constexpr size_t kRingSize = 64 * 1024;
constexpr uint32_t kLogDurationMs = 15000;

ulog::LittleFsSink* flash_sink = nullptr;
ulog::Lz4Sink* sink = nullptr;
ulog::AsyncWriter* logger = nullptr;
ulog::Message imu;
ulog::Message baro;
uint32_t started_ms = 0;
bool done = false;

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(2000);  // let the serial console attach

  uint8_t* ring_storage =
      static_cast<uint8_t*>(heap_caps_malloc(kRingSize, MALLOC_CAP_SPIRAM));
  if (ring_storage == nullptr) {
    ring_storage = static_cast<uint8_t*>(malloc(kRingSize));  // PSRAM fallback
  }
  if (ring_storage == nullptr) {
    Serial.println("ERROR: cannot allocate the log ring buffer");
    return;
  }

  flash_sink = new ulog::LittleFsSink("/log001.ulg.lz4");
  if (!flash_sink->open()) {
    Serial.println("ERROR: cannot open /log001.ulg.lz4 on LittleFS");
    return;
  }
  sink = new ulog::Lz4Sink(*flash_sink);  // compressed log: decompress on host
  if (!sink->is_valid()) {
    Serial.println("ERROR: cannot initialize LZ4 compression");
    return;
  }

  ulog::Writer::Config writer_config;  // default clock: esp_timer_get_time()
  ulog::AsyncWriter::Config async_config;
  async_config.ring_storage = ring_storage;
  async_config.ring_size = kRingSize;
  logger = new ulog::AsyncWriter(*sink, writer_config, async_config);
  if (!logger->start()) {
    Serial.println("ERROR: cannot start the drain task");
    return;
  }

  logger->add_info("sys_name", "ulog-esp32-example");
  logger->add_info("ver_hw", "seeed_xiao_esp32s3");
  logger->add_param("pid_kp", 1.5f);
  imu = logger->declare(
      "sensor_imu", "uint64_t timestamp;float[3] gyro_rad;float[3] accel_mps2;");
  baro = logger->declare(
      "sensor_baro", "uint64_t timestamp;float pressure_pa;float temperature_deg;");
  started_ms = millis();
}

void loop() {
  if (logger == nullptr || done) {
    delay(1000);
    return;
  }

  if (millis() - started_ms > kLogDurationMs) {
    logger->stop();
    sink->finish();
    const size_t written = flash_sink->size();
    flash_sink->close();
    Serial.printf("log complete: %u compressed bytes in /log001.ulg.lz4\n",
                  static_cast<unsigned>(written));
    Serial.printf("decompress on host: lz4 -d log001.ulg.lz4 log001.ulg\n");
    Serial.printf("littlefs used: %u bytes\n",
                  static_cast<unsigned>(LittleFS.usedBytes()));
    done = true;
    return;
  }

  static uint32_t n = 0;
  const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());

  ImuSample imu_sample{};
  imu_sample.timestamp = now;
  for (int i = 0; i < 3; ++i) {
    imu_sample.gyro_rad[i] = 0.1f * sinf(0.001f * n + i);
    imu_sample.accel_mps2[i] = 9.81f + 0.5f * cosf(0.0005f * n + i);
  }
  imu.log(imu_sample);

  if (n % 10 == 0) {
    BaroSample baro_sample{};
    baro_sample.timestamp = now;
    baro_sample.pressure_pa = 101325.0f - 100.0f * sinf(0.0002f * n);
    baro_sample.temperature_deg = 25.0f + 2.0f * sinf(0.0001f * n);
    baro.log(baro_sample);
  }
  if (n % 500 == 0) {
    logger->log_text(ulog::Level::Info, "example running");
  }
  ++n;
  delay(5);  // ~200 Hz imu, ~20 Hz baro
}
