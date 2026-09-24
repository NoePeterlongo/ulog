#include <unity.h>

#include "test_helpers.hpp"

#include "ulog/file_sink.hpp"
#include "ulog/lz4_sink.hpp"
#include "ulog/ram_sink.hpp"
#include "ulog/writer.hpp"

using namespace ulog_test;
using ulog::Lz4Sink;
using ulog::RamSink;
using ulog::Writer;

void test_lz4_frame() {
  RamSink inner;
  Lz4Sink lz4(inner);
  TEST_ASSERT_TRUE(lz4.is_valid());

  std::string repeated;
  for (int i = 0; i < 1000; ++i) repeated += "the quick brown fox ";
  TEST_ASSERT_TRUE(lz4.write(reinterpret_cast<const uint8_t*>(repeated.data()),
                             repeated.size()));
  TEST_ASSERT_TRUE(lz4.finish());

  const std::vector<uint8_t>& out = inner.bytes();
  const uint8_t magic[4] = {0x04, 0x22, 0x4D, 0x18};  // LZ4 frame magic
  TEST_ASSERT_TRUE(out.size() >= 4);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(magic, out.data(), 4);
  TEST_ASSERT_TRUE(out.size() < repeated.size());  // compressible data shrinks

  // After the footer no further write is accepted.
  TEST_ASSERT_FALSE(lz4.write(magic, sizeof(magic)));
}

void test_lz4_roundtrip_file() {
  g_now = 1000;  // same clock as the sync round-trip test
  ulog::FileSink file_sink("/tmp/ulog_lz4_roundtrip.ulg.lz4");
  TEST_ASSERT_TRUE(file_sink.is_open());
  Lz4Sink lz4(file_sink);
  TEST_ASSERT_TRUE(lz4.is_valid());
  Writer writer{lz4, test_config()};

  TEST_ASSERT_TRUE(writer.add_info("sys_name", "ulog-lib"));
  TEST_ASSERT_TRUE(writer.add_info("ver_sw_release", static_cast<uint32_t>(0x000100FF)));
  TEST_ASSERT_TRUE(writer.add_param("pid_kp", 1.5f));
  TEST_ASSERT_TRUE(writer.add_param("mode", static_cast<int32_t>(3)));

  auto imu = writer.declare(
      "sensor_imu", "uint64_t timestamp;float[3] gyro_rad;float[3] accel_mps2;");
  auto baro = writer.declare(
      "sensor_baro", "uint64_t timestamp;float pressure_pa;float temperature_deg;");
  TEST_ASSERT_TRUE(static_cast<bool>(imu));
  TEST_ASSERT_TRUE(static_cast<bool>(baro));

  ImuSample imu_sample{};
  for (int i = 0; i < 100; ++i) {
    imu_sample.timestamp = 10000 + 10 * i;
    for (int j = 0; j < 3; ++j) {
      imu_sample.gyro[j] = 0.1f * i + j;
      imu_sample.accel[j] = 9.81f + 0.01f * i;
    }
    TEST_ASSERT_TRUE(imu.log(imu_sample));
  }
  BaroSample baro_sample{};
  for (int i = 0; i < 50; ++i) {
    baro_sample.timestamp = 10000 + 20 * i;
    baro_sample.pressure_pa = 101325.0f - i;
    baro_sample.temperature_deg = 20.0f + 0.1f * i;
    TEST_ASSERT_TRUE(baro.log(baro_sample));
  }
  TEST_ASSERT_TRUE(writer.log_text(ulog::Level::Info, "host round-trip log"));
  TEST_ASSERT_TRUE(writer.log_text(ulog::Level::Warning, "synthetic data"));

  TEST_ASSERT_TRUE(lz4.finish());
  TEST_ASSERT_TRUE(file_sink.close());
}
