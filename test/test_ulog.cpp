#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#include <unity.h>

#include "test_helpers.hpp"

#include "ulog/ram_sink.hpp"
#include "ulog/file_sink.hpp"
#include "ulog/writer.hpp"

using namespace ulog_test;
using ulog::RamSink;
using ulog::Writer;

void test_header_and_flag_bits() {
  RamSink sink;
  Writer writer{sink, test_config()};

  const std::vector<uint8_t>& b = sink.bytes();
  TEST_ASSERT_EQUAL_UINT32(59, writer.bytes_written());
  const uint8_t expected_header[8] = {0x55, 0x4c, 0x6f, 0x67, 0x01, 0x12, 0x35, 0x01};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_header, b.data(), 8);
  TEST_ASSERT_EQUAL_UINT64(g_now, le64(b.data() + 8));

  Reader reader(b);
  Msg msg;
  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_UINT8('B', msg.type);
  TEST_ASSERT_EQUAL_UINT32(40, msg.payload.size());
  for (uint8_t byte : msg.payload) TEST_ASSERT_EQUAL_UINT8(0, byte);
  TEST_ASSERT_FALSE(reader.next(msg));
}

void test_declare_format_message() {
  RamSink sink;
  Writer writer{sink, test_config()};

  auto att = writer.declare("attitude", "uint64_t timestamp;float[4] q;");
  TEST_ASSERT_TRUE(static_cast<bool>(att));
  TEST_ASSERT_EQUAL_UINT16(24, att.payload_size());

  auto no_semicolon = writer.declare("baro", "uint64_t timestamp;float pressure");
  TEST_ASSERT_TRUE(static_cast<bool>(no_semicolon));

  Reader reader(sink.bytes());
  Msg msg;
  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_UINT8('B', msg.type);
  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_UINT8('F', msg.type);
  TEST_ASSERT_EQUAL_STRING("attitude:uint64_t timestamp;float[4] q;", payload_str(msg).c_str());
  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_UINT8('F', msg.type);
  TEST_ASSERT_EQUAL_STRING("baro:uint64_t timestamp;float pressure;", payload_str(msg).c_str());
}

void test_declare_rejections() {
  RamSink sink;
  Writer writer{sink, test_config()};

  TEST_ASSERT_FALSE(static_cast<bool>(writer.declare("bad_type", "uint64_t timestamp;float32 x;")));
  TEST_ASSERT_FALSE(static_cast<bool>(writer.declare("bad_name", "uint64_t timestamp;float x-y;")));
  TEST_ASSERT_FALSE(static_cast<bool>(writer.declare("unknown_nested", "uint64_t timestamp;vec3 v;")));
  TEST_ASSERT_FALSE(static_cast<bool>(writer.declare("float", "uint64_t timestamp;float x;")));
  TEST_ASSERT_FALSE(static_cast<bool>(writer.declare("", "uint64_t timestamp;")));
  TEST_ASSERT_FALSE(static_cast<bool>(writer.declare("bad chars", "uint64_t timestamp;")));
  TEST_ASSERT_FALSE(static_cast<bool>(writer.declare("empty", "")));

  auto good = writer.declare("good", "uint64_t timestamp;float x;");
  TEST_ASSERT_TRUE(static_cast<bool>(good));
  TEST_ASSERT_FALSE(static_cast<bool>(writer.declare("good", "uint64_t timestamp;float x;")));

  TEST_ASSERT_TRUE(writer.log_text(ulog::Level::Info, "data starts"));
  TEST_ASSERT_FALSE(static_cast<bool>(writer.declare("late", "uint64_t timestamp;float x;")));
}

void test_size_computation_and_padding() {
  RamSink sink;
  Writer writer{sink, test_config()};

  auto trailing = writer.declare(
      "trailing", "uint64_t timestamp;float[3] v;uint8_t[4] _padding;");
  TEST_ASSERT_TRUE(static_cast<bool>(trailing));
  TEST_ASSERT_EQUAL_UINT16(20, trailing.payload_size());

  auto middle = writer.declare(
      "middle", "uint64_t timestamp;uint8_t[4] _padding;float[3] v;");
  TEST_ASSERT_TRUE(static_cast<bool>(middle));
  TEST_ASSERT_EQUAL_UINT16(24, middle.payload_size());
}

void test_nested_format() {
  RamSink sink;
  Writer writer{sink, test_config()};

  auto vec3 = writer.declare("vec3", "float[3] v;");
  TEST_ASSERT_TRUE(static_cast<bool>(vec3));
  TEST_ASSERT_FALSE(vec3.log(ImuSample{}));  // nested-only: no timestamp field

  auto pairs = writer.declare("pairs", "uint64_t timestamp;vec3 a;vec3 b;");
  TEST_ASSERT_TRUE(static_cast<bool>(pairs));
  TEST_ASSERT_EQUAL_UINT16(32, pairs.payload_size());
}

void test_log_data_and_subscription() {
  RamSink sink;
  Writer writer{sink, test_config()};

  auto imu = writer.declare(
      "sensor_imu", "uint64_t timestamp;float[3] gyro_rad;float[3] accel_mps2;");
  auto baro = writer.declare(
      "sensor_baro", "uint64_t timestamp;float pressure_pa;float temperature_deg;");

  ImuSample imu_sample{};
  imu_sample.timestamp = 2000;
  TEST_ASSERT_TRUE(imu.log(imu_sample));

  BaroSample baro_sample{};
  baro_sample.timestamp = 2100;
  TEST_ASSERT_TRUE(baro.log(baro_sample));
  TEST_ASSERT_TRUE(imu.log(imu_sample));  // equal timestamp allowed

  Reader reader(sink.bytes());
  Msg msg;
  TEST_ASSERT_TRUE(reader.next(msg));  // 'B'
  TEST_ASSERT_TRUE(reader.next(msg));  // 'F' imu
  TEST_ASSERT_TRUE(reader.next(msg));  // 'F' baro

  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_UINT8('A', msg.type);
  TEST_ASSERT_EQUAL_UINT32(3 + strlen("sensor_imu"), msg.payload.size());
  TEST_ASSERT_EQUAL_UINT8(0, msg.payload[0]);
  TEST_ASSERT_EQUAL_UINT16(0, le16(msg.payload.data() + 1));
  TEST_ASSERT_EQUAL_STRING("sensor_imu", sub_str(msg, 3, msg.payload.size()).c_str());

  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_UINT8('D', msg.type);
  TEST_ASSERT_EQUAL_UINT32(2 + sizeof(ImuSample), msg.payload.size());
  TEST_ASSERT_EQUAL_UINT16(0, le16(msg.payload.data()));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(&imu_sample, msg.payload.data() + 2, sizeof(ImuSample));

  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_UINT8('A', msg.type);
  TEST_ASSERT_EQUAL_UINT16(1, le16(msg.payload.data() + 1));

  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_UINT8('D', msg.type);
  TEST_ASSERT_EQUAL_UINT16(1, le16(msg.payload.data()));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(&baro_sample, msg.payload.data() + 2, sizeof(BaroSample));

  TEST_ASSERT_TRUE(reader.next(msg));  // second imu 'D'
  TEST_ASSERT_EQUAL_UINT8('D', msg.type);
  TEST_ASSERT_FALSE(reader.next(msg));  // no further 'A'
}

void test_timestamp_not_first_field() {
  RamSink sink;
  Writer writer{sink, test_config()};

  struct __attribute__((packed)) Sample {
    float x;
    uint64_t timestamp;
  };
  static_assert(sizeof(Sample) == 12, "");

  auto topic = writer.declare("offset_ts", "float x;uint64_t timestamp;");
  TEST_ASSERT_TRUE(static_cast<bool>(topic));
  TEST_ASSERT_EQUAL_UINT16(12, topic.payload_size());

  Sample sample{1.5f, 5000};
  TEST_ASSERT_TRUE(topic.log(sample));

  Reader reader(sink.bytes());
  Msg msg;
  while (reader.next(msg)) {
    if (msg.type == 'D') {
      TEST_ASSERT_EQUAL_UINT32(2 + sizeof(Sample), msg.payload.size());
      TEST_ASSERT_EQUAL_UINT8_ARRAY(&sample, msg.payload.data() + 2, sizeof(Sample));
      return;
    }
  }
  TEST_FAIL_MESSAGE("no 'D' message found");
}

void test_monotonic_timestamps() {
  RamSink sink;
  Writer writer{sink, test_config()};

  auto imu = writer.declare(
      "sensor_imu", "uint64_t timestamp;float[3] gyro_rad;float[3] accel_mps2;");
  ImuSample sample{};
  sample.timestamp = 2000;
  TEST_ASSERT_TRUE(imu.log(sample));
  size_t size_ok = sink.bytes().size();

  sample.timestamp = 1999;
  TEST_ASSERT_FALSE(imu.log(sample));
  TEST_ASSERT_EQUAL_UINT32(size_ok, sink.bytes().size());

  sample.timestamp = 2000;  // equal is allowed
  TEST_ASSERT_TRUE(imu.log(sample));
}

void test_log_text() {
  RamSink sink;
  Writer writer{sink, test_config()};

  TEST_ASSERT_TRUE(writer.log_text(ulog::Level::Warning, "low battery"));
  TEST_ASSERT_FALSE(writer.log_text(ulog::Level::Info, "abc", 100));

  Reader reader(sink.bytes());
  Msg msg;
  TEST_ASSERT_TRUE(reader.next(msg));  // 'B'
  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_UINT8('L', msg.type);
  TEST_ASSERT_EQUAL_UINT32(9 + strlen("low battery"), msg.payload.size());
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ulog::Level::Warning), msg.payload[0]);
  TEST_ASSERT_EQUAL_UINT64(g_now, le64(msg.payload.data() + 1));
  TEST_ASSERT_EQUAL_STRING("low battery", sub_str(msg, 9, msg.payload.size()).c_str());
}

void test_info_and_params() {  RamSink sink;
  Writer writer{sink, test_config()};

  TEST_ASSERT_TRUE(writer.add_info("sys_name", "myproj"));
  TEST_ASSERT_TRUE(writer.add_info("ver_sw_release", static_cast<uint32_t>(0x000100FF)));
  TEST_ASSERT_TRUE(writer.add_param("pid_kp", 1.5f));
  TEST_ASSERT_TRUE(writer.add_param("mode", static_cast<int32_t>(3)));
  TEST_ASSERT_FALSE(writer.add_info("bad name!", "x"));
  TEST_ASSERT_FALSE(writer.add_info("", "x"));

  Reader reader(sink.bytes());
  Msg msg;
  TEST_ASSERT_TRUE(reader.next(msg));  // 'B'

  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_UINT8('I', msg.type);
  TEST_ASSERT_EQUAL_UINT8(16, msg.payload[0]);  // "char[6] sys_name"
  TEST_ASSERT_EQUAL_STRING("char[6] sys_name", sub_str(msg, 1, 17).c_str());
  TEST_ASSERT_EQUAL_STRING("myproj", sub_str(msg, 17, msg.payload.size()).c_str());

  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_UINT8('I', msg.type);
  TEST_ASSERT_EQUAL_STRING("uint32_t ver_sw_release", sub_str(msg, 1, 24).c_str());
  TEST_ASSERT_EQUAL_UINT32(0x000100FF, le32(msg.payload.data() + 24));

  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_UINT8('P', msg.type);
  TEST_ASSERT_EQUAL_STRING("float pid_kp", sub_str(msg, 1, 13).c_str());
  float kp = 0;
  memcpy(&kp, msg.payload.data() + 13, sizeof(kp));
  TEST_ASSERT_EQUAL_FLOAT(1.5f, kp);

  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_UINT8('P', msg.type);
  TEST_ASSERT_EQUAL_STRING("int32_t mode", sub_str(msg, 1, 13).c_str());
  TEST_ASSERT_EQUAL_UINT32(3, le32(msg.payload.data() + 13));

  // 'I'/'P' stay valid in the data section.
  TEST_ASSERT_TRUE(writer.log_text(ulog::Level::Info, "data"));
  TEST_ASSERT_TRUE(writer.add_param("pid_kp", 2.0f));
}

void test_sync_messages() {
  RamSink sink;
  Writer::Config config = test_config();
  config.sync_interval_bytes = 16;
  Writer writer{sink, config};

  auto imu = writer.declare(
      "sensor_imu", "uint64_t timestamp;float[3] gyro_rad;float[3] accel_mps2;");
  ImuSample sample{};
  for (int i = 0; i < 20; ++i) {
    sample.timestamp = 2000 + i;
    TEST_ASSERT_TRUE(imu.log(sample));
  }

  const uint8_t sync_magic[8] = {0x2F, 0x73, 0x13, 0x20, 0x25, 0x0C, 0xBB, 0x12};
  Reader reader(sink.bytes());
  Msg msg;
  int sync_count = 0;
  while (reader.next(msg)) {
    if (msg.type == 'S') {
      TEST_ASSERT_EQUAL_UINT32(8, msg.payload.size());
      TEST_ASSERT_EQUAL_UINT8_ARRAY(sync_magic, msg.payload.data(), 8);
      ++sync_count;
    }
  }
  TEST_ASSERT_TRUE(sync_count >= 5);
}

void test_roundtrip_file() {
  ulog::FileSink sink("/tmp/ulog_roundtrip.ulg");
  TEST_ASSERT_TRUE(sink.is_open());
  Writer writer{sink, test_config()};

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

  TEST_ASSERT_TRUE(sink.sync());
  TEST_ASSERT_TRUE(sink.close());
}

void test_info_numeric_types() {
  RamSink sink;
  Writer writer{sink, test_config()};

  TEST_ASSERT_TRUE(writer.add_info("i8", static_cast<int8_t>(-5)));
  TEST_ASSERT_TRUE(writer.add_info("flag", true));
  TEST_ASSERT_TRUE(writer.add_info("big", static_cast<uint64_t>(0x1122334455667788ULL)));
  TEST_ASSERT_TRUE(writer.add_info("ratio", 2.5f));
  TEST_ASSERT_TRUE(writer.add_info("pi", 3.5));
  TEST_ASSERT_TRUE(writer.add_info("u8", static_cast<uint8_t>(200)));

  Reader reader(sink.bytes());
  Msg msg;
  TEST_ASSERT_TRUE(reader.next(msg));  // 'B'

  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_UINT8('I', msg.type);
  TEST_ASSERT_EQUAL_STRING("int8_t i8", sub_str(msg, 1, 10).c_str());
  TEST_ASSERT_EQUAL_UINT8(0xFB, msg.payload[10]);

  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_STRING("bool flag", sub_str(msg, 1, 10).c_str());
  TEST_ASSERT_EQUAL_UINT8(1, msg.payload[10]);

  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_STRING("uint64_t big", sub_str(msg, 1, 13).c_str());
  TEST_ASSERT_EQUAL_UINT64(0x1122334455667788ULL, le64(msg.payload.data() + 13));

  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_STRING("float ratio", sub_str(msg, 1, 12).c_str());
  float ratio = 0;
  memcpy(&ratio, msg.payload.data() + 12, sizeof(ratio));
  TEST_ASSERT_EQUAL_FLOAT(2.5f, ratio);

  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_STRING("double pi", sub_str(msg, 1, 10).c_str());
  double pi = 0;
  memcpy(&pi, msg.payload.data() + 10, sizeof(pi));
  TEST_ASSERT_EQUAL_FLOAT(3.5, pi);

  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_STRING("uint8_t u8", sub_str(msg, 1, 11).c_str());
  TEST_ASSERT_EQUAL_UINT8(200, msg.payload[11]);

  TEST_ASSERT_FALSE(reader.next(msg));
}

void test_add_info_limits() {
  RamSink sink;
  Writer writer{sink, test_config()};

  std::string name_23(23, 'n');
  std::string name_24(24, 'n');
  std::string value_150(150, 'v');
  std::string value_200(200, 'v');

  TEST_ASSERT_TRUE(writer.add_info(name_23.c_str(), value_150.c_str()));
  TEST_ASSERT_FALSE(writer.add_info(name_24.c_str(), value_150.c_str()));
  TEST_ASSERT_FALSE(writer.add_info("ok", value_200.c_str()));
  TEST_ASSERT_FALSE(writer.add_info("bad-name!", "x"));
}

void test_declare_limits() {
  RamSink sink;
  Writer writer{sink, test_config()};

  int accepted = 0;
  for (int i = 0; i < 33; ++i) {
    char name[8];
    snprintf(name, sizeof(name), "f%02d", i);
    if (writer.declare(name, "uint64_t timestamp;")) ++accepted;
  }
  TEST_ASSERT_EQUAL_INT(32, accepted);  // registry full after kMaxFormats

  RamSink sink2;
  Writer writer2{sink2, test_config()};
  const std::string name_23(23, 'n');
  const std::string name_24(24, 'n');
  TEST_ASSERT_TRUE(static_cast<bool>(writer2.declare(name_23.c_str(), "uint64_t timestamp;")));
  TEST_ASSERT_FALSE(static_cast<bool>(writer2.declare(name_24.c_str(), "uint64_t timestamp;")));

  // payload must stay under the ULog 16-bit message size
  TEST_ASSERT_FALSE(static_cast<bool>(writer2.declare("toobig", "uint64_t timestamp;float[16384] v;")));
  auto okbig = writer2.declare("okbig", "uint64_t timestamp;float[16381] v;");
  TEST_ASSERT_TRUE(static_cast<bool>(okbig));
  TEST_ASSERT_EQUAL_UINT16(8 + 4 * 16381, okbig.payload_size());

  // parser rejects absurd array lengths before any size math
  TEST_ASSERT_FALSE(static_cast<bool>(writer2.declare("bigarray", "uint64_t timestamp;uint8_t[100001] x;")));
}

void test_log_text_empty() {
  RamSink sink;
  Writer writer{sink, test_config()};

  TEST_ASSERT_TRUE(writer.log_text(ulog::Level::Debug, ""));

  Reader reader(sink.bytes());
  Msg msg;
  TEST_ASSERT_TRUE(reader.next(msg));  // 'B'
  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_UINT8('L', msg.type);
  TEST_ASSERT_EQUAL_UINT32(9, msg.payload.size());
  TEST_ASSERT_EQUAL_UINT8(7, msg.payload[0]);
  TEST_ASSERT_EQUAL_UINT64(g_now, le64(msg.payload.data() + 1));
  TEST_ASSERT_FALSE(reader.next(msg));
}

void test_writer_reports_sink_failure() {
  FailingSink sink;
  Writer writer{sink, test_config()};

  TEST_ASSERT_FALSE(writer.log_text(ulog::Level::Info, "x"));
  TEST_ASSERT_FALSE(static_cast<bool>(writer.declare("t", "uint64_t timestamp;float v;")));
  TEST_ASSERT_FALSE(writer.add_param("p", 1.0f));
  TEST_ASSERT_FALSE(writer.add_info("i", "v"));
  TEST_ASSERT_FALSE(writer.log_dropout(10));

  auto topic = writer.declare("t2", "uint64_t timestamp;float v;");
  TEST_ASSERT_FALSE(static_cast<bool>(topic));  // 'F' write failed too
}
