#include <unity.h>

#include "test_helpers.hpp"

#include "ulog/file_sink.hpp"
#include "ulog/ring.hpp"

using namespace ulog_test;
using ulog::RamSink;
using ulog::RingBuffer;
using ulog::RingWriterTarget;
using ulog::Writer;

void test_ring_write_read_wrap() {
  uint8_t storage[16];
  RingBuffer ring(storage, sizeof(storage));

  const uint8_t first_msg[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  ring.start_message();
  TEST_ASSERT_TRUE(ring.write(first_msg, sizeof(first_msg)));
  ring.commit_message();
  TEST_ASSERT_EQUAL_UINT32(10, ring.used());

  uint8_t out[16];
  TEST_ASSERT_EQUAL_UINT32(6, ring.read(out, 6));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(first_msg, out, 6);

  const uint8_t second_msg[10] = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
  ring.start_message();  // straddles the wrap boundary
  TEST_ASSERT_TRUE(ring.write(second_msg, sizeof(second_msg)));
  ring.commit_message();
  TEST_ASSERT_EQUAL_UINT32(14, ring.used());

  TEST_ASSERT_EQUAL_UINT32(14, ring.read(out, sizeof(out)));
  const uint8_t expected[14] = {6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, 14);
  TEST_ASSERT_EQUAL_UINT32(0, ring.used());
}

void test_ring_message_rollback() {
  uint8_t storage[8];
  RingBuffer ring(storage, sizeof(storage));

  ring.start_message();
  TEST_ASSERT_TRUE(ring.write(reinterpret_cast<const uint8_t*>("abc"), 3));
  ring.commit_message();

  ring.start_message();
  TEST_ASSERT_TRUE(ring.write(reinterpret_cast<const uint8_t*>("de"), 2));
  TEST_ASSERT_FALSE(ring.write(reinterpret_cast<const uint8_t*>("fghij"), 5));
  ring.rollback_message();
  TEST_ASSERT_EQUAL_UINT32(3, ring.used());

  ring.start_message();
  TEST_ASSERT_TRUE(ring.write(reinterpret_cast<const uint8_t*>("fgh"), 3));
  ring.commit_message();

  uint8_t out[8];
  TEST_ASSERT_EQUAL_UINT32(6, ring.read(out, sizeof(out)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY("abcfgh", out, 6);
}

void test_writer_ring_atomic_drops() {
  uint8_t storage[256];
  RingBuffer ring(storage, sizeof(storage));
  RingWriterTarget target(ring, test_now);
  Writer writer(target, test_config(), nullptr);

  auto imu = writer.declare(
      "sensor_imu", "uint64_t timestamp;float[3] gyro_rad;float[3] accel_mps2;");
  TEST_ASSERT_TRUE(static_cast<bool>(imu));

  ImuSample sample{};
  int logged = 0;
  for (int i = 0; i < 40; ++i) {
    sample.timestamp = 2000 + i;
    if (imu.log(sample)) ++logged;
  }
  TEST_ASSERT_TRUE(logged > 0);
  TEST_ASSERT_TRUE(logged < 40);  // ring smaller than 40 messages: drops
  TEST_ASSERT_EQUAL_UINT32(0, target.take_dropped_ms());  // window still open

  g_now += 5000;  // close the dropout window

  RamSink drained;
  uint8_t buf[64];
  size_t n;
  while ((n = ring.read(buf, sizeof(buf))) > 0) {
    TEST_ASSERT_TRUE(drained.write(buf, n));
  }

  sample.timestamp = 2040;
  TEST_ASSERT_TRUE(imu.log(sample));  // fits again after the drain
  TEST_ASSERT_EQUAL_UINT32(5, target.take_dropped_ms());

  while ((n = ring.read(buf, sizeof(buf))) > 0) {
    TEST_ASSERT_TRUE(drained.write(buf, n));
  }

  // The drained stream contains only whole messages.
  Reader reader(drained.bytes());
  Msg msg;
  int data_count = 0;
  while (reader.next(msg)) {
    if (msg.type == 'D') ++data_count;
  }
  TEST_ASSERT_EQUAL_INT(logged + 1, data_count);
}

void test_writer_log_dropout() {
  RamSink sink;
  Writer writer{sink, test_config()};

  TEST_ASSERT_TRUE(writer.log_dropout(70000));
  TEST_ASSERT_TRUE(writer.log_dropout(0));  // no message for zero

  Reader reader(sink.bytes());
  Msg msg;
  TEST_ASSERT_TRUE(reader.next(msg));  // 'B'
  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_UINT8('O', msg.type);
  TEST_ASSERT_EQUAL_UINT32(2, msg.payload.size());
  TEST_ASSERT_EQUAL_UINT16(0xFFFF, le16(msg.payload.data()));
  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_UINT8('O', msg.type);
  TEST_ASSERT_EQUAL_UINT16(70000 - 0xFFFF, le16(msg.payload.data()));
  TEST_ASSERT_FALSE(reader.next(msg));
}

void test_async_roundtrip_file() {
  uint8_t storage[512];
  RingBuffer ring(storage, sizeof(storage));
  RingWriterTarget target(ring, test_now);
  Writer writer(target, test_config(), nullptr);
  ulog::FileSink sink("/tmp/ulog_async_roundtrip.ulg");
  TEST_ASSERT_TRUE(sink.is_open());

  auto imu = writer.declare(
      "sensor_imu", "uint64_t timestamp;float[3] gyro_rad;float[3] accel_mps2;");
  TEST_ASSERT_TRUE(static_cast<bool>(imu));
  TEST_ASSERT_TRUE(writer.add_info("sys_name", "ulog-async"));

  ImuSample sample{};
  int logged = 0;
  for (int i = 0; i < 200; ++i) {
    sample.timestamp = 10000 + i;
    sample.gyro[0] = 0.01f * i;
    g_now = 1000000 + 1000 * static_cast<uint64_t>(i);
    if (imu.log(sample)) ++logged;
  }
  TEST_ASSERT_TRUE(logged < 200);  // no drain: overflow guaranteed

  uint8_t buf[128];
  size_t n;
  while ((n = ring.read(buf, sizeof(buf))) > 0) {
    TEST_ASSERT_TRUE(sink.write(buf, n));
  }

  sample.timestamp = 10200;
  g_now = 1200000;
  TEST_ASSERT_TRUE(imu.log(sample));  // closes the dropout window

  const uint32_t dropped = target.take_dropped_ms();
  TEST_ASSERT_TRUE(dropped > 0);
  TEST_ASSERT_TRUE(writer.log_dropout(dropped));

  while ((n = ring.read(buf, sizeof(buf))) > 0) {
    TEST_ASSERT_TRUE(sink.write(buf, n));
  }

  TEST_ASSERT_TRUE(sink.sync());
  TEST_ASSERT_TRUE(sink.close());
}
