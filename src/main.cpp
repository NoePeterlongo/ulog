#include <Arduino.h>
#include <esp_timer.h>
#include <math.h>

#include "logger.hpp"

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

// Two flights per boot: takeoff, land, take off again, land, done.
constexpr uint32_t kFlightMs[] = {15000, 10000};
constexpr uint32_t kGroundPauseMs = 4000;

// Logger goes into the application/hfsm2 context; here a global stands in.
Logger* logger = nullptr;
ulog::Message imu;
ulog::Message baro;

int session = 0;
uint32_t session_started_ms = 0;
uint32_t ground_until_ms = 0;
bool done = false;

bool start_session() {
  if (!logger->begin()) return false;
  logger->add_info("sys_name", "ulog-esp32-example");
  logger->add_param("pid_kp", 1.5f);
  imu = logger->declare(
      "sensor_imu", "uint64_t timestamp;float[3] gyro_rad;float[3] accel_mps2;");
  baro = logger->declare(
      "sensor_baro", "uint64_t timestamp;float pressure_pa;float temperature_deg;");
  session_started_ms = millis();
  return true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(2000);  // let the serial console attach

  // %03d: each flight gets its own file (log001, log002, ...)
  logger = new Logger("/log%03d.ulg.lz4");
  if (!start_session()) {
    Serial.println("ERROR: cannot start the recording session");
  }
}

void loop() {
  if (logger == nullptr || done) {
    delay(1000);
    return;
  }

  // "landed": persist, then either pause on the ground or stop
  if (logger->running() && millis() - session_started_ms > kFlightMs[session]) {
    const size_t in_ram = logger->bytes_logged();
    if (!logger->write_to_flash()) {
      Serial.println("ERROR: persisting to flash failed");
    }
    Serial.printf("flight %d persisted: %u bytes in RAM -> %u bytes in %s\n",
                  session + 1, static_cast<unsigned>(in_ram),
                  static_cast<unsigned>(logger->last_flash_bytes()),
                  logger->session_path());
    ++session;
    if (session >= 2) {
      Serial.println("all flights recorded, pulling off device with "
                     "tools/pull_littlefs.py");
      done = true;
      return;
    }
    ground_until_ms = millis() + kGroundPauseMs;
    return;
  }

  // on the ground between flights
  if (!logger->running()) {
    if (millis() >= ground_until_ms) {
      if (!start_session()) {
        Serial.println("ERROR: cannot re-start the recording session");
        done = true;
      }
    }
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
