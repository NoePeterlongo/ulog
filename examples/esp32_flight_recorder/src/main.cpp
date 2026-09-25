#include <Arduino.h>
#include <esp_timer.h>
#include <math.h>

#include "ulog/logger.hpp"

namespace {

// ---------------------------------------------------------------------
// The project-specific logger: a derived class that declares its messages
// once and exposes simple logging methods to every task. Tasks never see
// formats, sinks, files or thread-safety.
class MyLogger : public ulog::LoggerBase {
 public:
  MyLogger() : LoggerBase("/log%03d.ulg") {}  // one file per boot, %03d

  void log_imu(uint64_t timestamp, const float gyro[3], const float accel[3]) {
    ImuSample s{timestamp,
                {gyro[0], gyro[1], gyro[2]},
                {accel[0], accel[1], accel[2]}};
    imu_.log(s);
  }

  void log_baro(uint64_t timestamp, float pressure_pa, float temperature_deg) {
    baro_.log(BaroSample{timestamp, pressure_pa, temperature_deg});
  }

  void log_flight_phase(const char* phase) {
    log_text(ulog::Level::Notice, phase);
  }

 protected:
  bool on_declare() override {
    imu_ = declare("sensor_imu",
                   "uint64_t timestamp;float[3] gyro_rad;float[3] accel_mps2;");
    baro_ = declare("sensor_baro",
                    "uint64_t timestamp;float pressure_pa;float temperature_deg;");
    if (!static_cast<bool>(imu_) || !static_cast<bool>(baro_)) return false;
    add_info("sys_name", "ulog-esp32-example");
    add_info("ver_hw", "seeed_xiao_esp32s3");
    add_param("pid_kp", 1.5f);
    return true;
  }

 private:
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

  ulog::Message imu_;
  ulog::Message baro_;
};

// ---------------------------------------------------------------------
// Demo flight plan: two flights per boot, persist() at each landing.
// 'd' on the serial console prints the debug dump on demand.
constexpr uint32_t kFlightMs[] = {15000, 10000};
constexpr uint32_t kGroundPauseMs = 4000;

MyLogger logger;
int session = 0;
bool in_flight = false;
uint32_t session_started_ms = 0;
uint32_t ground_until_ms = 0;
bool done = false;

bool start_session() {
  if (!logger.begin()) return false;  // first call: declares the messages
  logger.log_flight_phase("takeoff");
  in_flight = true;
  session_started_ms = millis();
  return true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(2000);  // let the serial console attach

  if (!start_session()) {
    Serial.println("ERROR: logger begin failed (RAM or declare)");
  }
}

void loop() {
  if (done) {
    if (Serial.available() && Serial.read() == 'd') {
      logger.debug_dump(Serial);
    }
    delay(10);
    return;
  }

  // "landed": persist the flight, then pause on the ground or stop
  if (in_flight && millis() - session_started_ms > kFlightMs[session]) {
    logger.log_flight_phase("landing");
    const size_t written = logger.persist();  // RAM -> flash, one burst
    in_flight = false;
    Serial.printf("flight %d persisted: %u bytes -> %s (total %u)\n",
                  session + 1, static_cast<unsigned>(written),
                  logger.session_path(),
                  static_cast<unsigned>(logger.persisted_bytes()));
    ++session;
    if (session >= 2) {
      logger.end();
      logger.debug_dump(Serial);
      Serial.println("send 'd' for the debug dump");
      done = true;
      return;
    }
    ground_until_ms = millis() + kGroundPauseMs;
    return;
  }

  // on the ground between flights
  if (!in_flight) {
    if (Serial.available() && Serial.read() == 'e') {
      // demo of the erase path: formats the partition, then a fresh stream
      if (logger.erase_flash()) {
        Serial.println("flash erased");
        if (logger.restart()) Serial.println("fresh stream started");
      } else {
        Serial.println("erase failed");
      }
    }
    if (millis() >= ground_until_ms && !start_session()) {
      Serial.println("ERROR: cannot re-start the session");
      done = true;
    }
    return;
  }

  static uint32_t n = 0;
  const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());

  const float gyro[3] = {0.1f * sinf(0.001f * n),
                         0.1f * sinf(0.001f * n + 1),
                         0.1f * sinf(0.001f * n + 2)};
  const float accel[3] = {9.81f + 0.5f * cosf(0.0005f * n),
                          9.81f + 0.5f * cosf(0.0005f * n + 1),
                          9.81f + 0.5f * cosf(0.0005f * n + 2)};
  logger.log_imu(now, gyro, accel);

  if (n % 10 == 0) {
    logger.log_baro(now, 101325.0f - 100.0f * sinf(0.0002f * n),
                    25.0f + 2.0f * sinf(0.0001f * n));
  }
  ++n;
  delay(5);  // ~200 Hz imu, ~20 Hz baro
}
