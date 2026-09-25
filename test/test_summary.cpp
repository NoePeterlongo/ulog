#include <unity.h>

#include <string>
#include <vector>

#include "test_helpers.hpp"

#include "ulog/log_summary.hpp"

using namespace ulog_test;
using ulog::RamSink;
using ulog::Writer;

namespace {

struct Collector {
  std::vector<std::string> lines;
};

void collect(void* ctx, const char* line) {
  static_cast<Collector*>(ctx)->lines.push_back(line);
}

bool contains(const Collector& c, const char* needle) {
  for (const auto& line : c.lines) {
    if (line.find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

void test_log_summary_stats() {
  RamSink sink;
  Writer writer{sink, test_config()};

  TEST_ASSERT_TRUE(writer.add_info("sys_name", "test-project"));
  TEST_ASSERT_TRUE(writer.add_param("pid_kp", 1.5f));
  auto imu = writer.declare(
      "sensor_imu", "uint64_t timestamp;float[3] gyro_rad;float[3] accel_mps2;");
  auto baro = writer.declare(
      "sensor_baro", "uint64_t timestamp;float pressure_pa;float temperature_deg;");
  TEST_ASSERT_TRUE(static_cast<bool>(imu));
  TEST_ASSERT_TRUE(static_cast<bool>(baro));

  ImuSample imu_sample{};
  for (int i = 0; i < 40; ++i) {
    imu_sample.timestamp = 1000 + i;
    TEST_ASSERT_TRUE(imu.log(imu_sample));
  }
  BaroSample baro_sample{};
  for (int i = 0; i < 7; ++i) {
    baro_sample.timestamp = 1000 + i;
    TEST_ASSERT_TRUE(baro.log(baro_sample));
  }
  TEST_ASSERT_TRUE(writer.log_text(ulog::Level::Warning, "low battery"));
  TEST_ASSERT_TRUE(writer.log_dropout(1234));

  Collector collector;
  LogSummary s = summarize_ulog(sink.bytes().data(), sink.bytes().size(),
                               collect, &collector);

  TEST_ASSERT_EQUAL_UINT32(sink.bytes().size(), s.bytes);
  TEST_ASSERT_EQUAL_UINT16(2, s.formats);
  TEST_ASSERT_EQUAL_UINT16(2, s.subscriptions);
  TEST_ASSERT_EQUAL_UINT32(47, s.samples);
  TEST_ASSERT_EQUAL_UINT32(1, s.infos);
  TEST_ASSERT_EQUAL_UINT32(1, s.params);
  TEST_ASSERT_EQUAL_UINT32(1, s.texts);
  TEST_ASSERT_EQUAL_UINT32(1234, s.dropout_ms);
  TEST_ASSERT_FALSE(s.truncated);

  TEST_ASSERT_TRUE(contains(collector, "format: sensor_imu"));
  TEST_ASSERT_TRUE(contains(collector, "subscribe: sensor_imu (id 0)"));
  TEST_ASSERT_TRUE(contains(collector, "info: char[12] sys_name = test-project"));
  TEST_ASSERT_TRUE(contains(collector, "param: float pid_kp = 1.5"));
  TEST_ASSERT_TRUE(contains(collector, "text[WARN]: low battery"));
  TEST_ASSERT_TRUE(contains(collector, "dropout: 1234 ms"));
  TEST_ASSERT_TRUE(contains(collector, "data: sensor_imu: 40 samples"));
  TEST_ASSERT_TRUE(contains(collector, "data: sensor_baro: 7 samples"));
}

void test_log_summary_truncated() {
  std::vector<uint8_t> bytes(10, 0);  // shorter than the 16-byte file header
  bytes[0] = 0x55;

  Collector collector;
  LogSummary s = summarize_ulog(bytes.data(), bytes.size(), collect, &collector);
  TEST_ASSERT_TRUE(s.truncated);
  TEST_ASSERT_EQUAL_UINT32(0, s.formats);

  // stream cut in the middle of a message
  RamSink sink;
  Writer writer{sink, test_config()};
  auto imu = writer.declare(
      "sensor_imu", "uint64_t timestamp;float[3] gyro_rad;float[3] accel_mps2;");
  TEST_ASSERT_TRUE(static_cast<bool>(imu));
  TEST_ASSERT_TRUE(imu.log(ImuSample{1000, {1, 2, 3}, {4, 5, 6}}));
  TEST_ASSERT_TRUE(imu.log(ImuSample{1001, {1, 2, 3}, {4, 5, 6}}));

  std::vector<uint8_t> cut(sink.bytes().begin(), sink.bytes().end() - 10);
  LogSummary cut_summary = summarize_ulog(cut.data(), cut.size(), collect, &collector);
  TEST_ASSERT_TRUE(cut_summary.truncated);
}

void test_log_summary_streaming_chunks() {
  // feed() must reassemble messages split across arbitrary chunk boundaries
  RamSink sink;
  Writer writer{sink, test_config()};
  auto imu = writer.declare(
      "sensor_imu", "uint64_t timestamp;float[3] gyro_rad;float[3] accel_mps2;");
  TEST_ASSERT_TRUE(static_cast<bool>(imu));
  TEST_ASSERT_TRUE(writer.add_param("pid_kp", 2.5f));
  for (int i = 0; i < 25; ++i) {
    TEST_ASSERT_TRUE(imu.log(ImuSample{1000 + i, {1, 2, 3}, {4, 5, 6}}));
  }

  Collector whole;
  LogSummary reference = summarize_ulog(sink.bytes().data(), sink.bytes().size(),
                                        collect, &whole);

  for (size_t chunk_size : {size_t(1), size_t(7), size_t(64), size_t(10000)}) {
    Collector streamed;
    UlogSummary walker(collect, &streamed);
    for (size_t i = 16; i < sink.bytes().size(); i += chunk_size) {
      const size_t n = (i + chunk_size < sink.bytes().size())
                          ? chunk_size
                          : sink.bytes().size() - i;
      walker.feed(sink.bytes().data() + i, n);
    }
    LogSummary s = walker.finish();
    s.bytes = reference.bytes;
    TEST_ASSERT_EQUAL_UINT16(reference.formats, s.formats);
    TEST_ASSERT_EQUAL_UINT16(reference.subscriptions, s.subscriptions);
    TEST_ASSERT_EQUAL_UINT32(reference.samples, s.samples);
    TEST_ASSERT_EQUAL_UINT32(reference.params, s.params);
    TEST_ASSERT_FALSE(s.truncated);
    TEST_ASSERT_TRUE(contains(streamed, "data: sensor_imu: 25 samples"));
    TEST_ASSERT_TRUE(contains(streamed, "param: float pid_kp = 2.5"));
  }
}
