#include <unity.h>

#include <string>
#include <vector>

#include "test_helpers.hpp"

#include "ulog/ram_sink.hpp"
#include "ulog/spool_sink.hpp"
#include "ulog/writer.hpp"

using namespace ulog_test;
using ulog::SpoolSink;
using ulog::Writer;

namespace {

struct __attribute__((packed)) Sample {
  uint64_t timestamp;
  float v;
};

const char* kFields = "uint64_t timestamp;float v;";

}  // namespace

void test_spool_capacity_and_rotate() {
  uint8_t buf_a[512];
  uint8_t buf_b[512];
  SpoolSink spool(buf_a, buf_b, sizeof(buf_a), test_now);
  Writer writer(spool, test_config(), nullptr);

  auto topic = writer.declare("t", kFields);
  TEST_ASSERT_TRUE(static_cast<bool>(topic));

  Sample sample{1000, 1.0f};
  TEST_ASSERT_TRUE(topic.log(sample));
  TEST_ASSERT_TRUE(topic.log(Sample{1001, 2.0f}));
  TEST_ASSERT_TRUE(spool.pending() > 16 + 40 + 40);  // header + 'F' + 'A' + 2 'D'

  size_t size = 0;
  const uint8_t* bytes = spool.rotate(&size);
  TEST_ASSERT_TRUE(size > 0);
  TEST_ASSERT_EQUAL_UINT32(0, spool.pending());  // second buffer is active now

  // the rotated-out stream parses: exactly the expected messages
  std::vector<uint8_t> stream(bytes, bytes + size);
  Reader reader(stream);
  Msg msg;
  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_UINT8('B', msg.type);
  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_UINT8('F', msg.type);
  TEST_ASSERT_TRUE(reader.next(msg));
  TEST_ASSERT_EQUAL_UINT8('A', msg.type);
  int data_count = 0;
  while (reader.next(msg)) {
    if (msg.type == 'D') ++data_count;
  }
  TEST_ASSERT_EQUAL_INT(2, data_count);
  TEST_ASSERT_EQUAL_UINT32(0, spool.take_dropped_ms());
}

void test_spool_atomic_drops() {
  // header(16) + 'B'(43) + 'F'("t:"+fields, ~31) + 'A'(7) + 'D'(21) each
  uint8_t buf_a[128];
  uint8_t buf_b[128];
  SpoolSink spool(buf_a, buf_b, sizeof(buf_a), test_now);
  Writer writer(spool, test_config(), nullptr);

  auto topic = writer.declare("t", kFields);
  TEST_ASSERT_TRUE(static_cast<bool>(topic));

  TEST_ASSERT_TRUE(topic.log(Sample{2000, 1.0f}));   // fits
  TEST_ASSERT_FALSE(topic.log(Sample{2001, 2.0f}));  // no room: whole message dropped
  TEST_ASSERT_FALSE(topic.log(Sample{2002, 3.0f}));  // still full
  TEST_ASSERT_EQUAL_UINT32(0, spool.take_dropped_ms());  // window still open

  size_t size = 0;
  const uint8_t* bytes = spool.rotate(&size);
  std::vector<uint8_t> stream(bytes, bytes + size);
  Reader reader(stream);
  Msg msg;
  int data_count = 0;
  while (reader.next(msg)) {
    if (msg.type == 'D') ++data_count;
  }
  TEST_ASSERT_EQUAL_INT(1, data_count);  // no partial garbage in the stream

  g_now += 2500;  // close the drop window
  TEST_ASSERT_TRUE(topic.log(Sample{2003, 4.0f}));
  TEST_ASSERT_EQUAL_UINT32(2, spool.take_dropped_ms());  // 2.5 ms lost
  TEST_ASSERT_EQUAL_UINT32(0, spool.take_dropped_ms());
}

void test_spool_watermark() {
  uint8_t buf_a[64];
  uint8_t buf_b[64];
  SpoolSink spool(buf_a, buf_b, sizeof(buf_a), test_now);

  TEST_ASSERT_FALSE(spool.near_full(50));
  spool.start_message();
  TEST_ASSERT_TRUE(spool.write(reinterpret_cast<const uint8_t*>("0123456789"), 10));
  TEST_ASSERT_TRUE(spool.finish_message(true));
  TEST_ASSERT_EQUAL_UINT32(10, spool.pending());
  TEST_ASSERT_TRUE(spool.near_full(8));    // 10 >= 8
  TEST_ASSERT_FALSE(spool.near_full(11));  // 10 < 11
  TEST_ASSERT_EQUAL_UINT32(64, spool.capacity());

  // zero-capacity spool: never accepts, never crashes
  SpoolSink empty(nullptr, nullptr, 0, test_now);
  TEST_ASSERT_FALSE(empty.write(reinterpret_cast<const uint8_t*>("x"), 1));
  size_t size = 99;
  TEST_ASSERT_NULL(empty.rotate(&size));
  TEST_ASSERT_EQUAL_UINT32(0, size);
  TEST_ASSERT_NULL(empty.peek(&size));
  TEST_ASSERT_EQUAL_UINT32(0, size);
}

void test_spool_two_buffers_independent() {
  uint8_t buf_a[256];
  uint8_t buf_b[256];
  SpoolSink spool(buf_a, buf_b, sizeof(buf_a), test_now);
  Writer writer(spool, test_config(), nullptr);

  auto topic = writer.declare("t", kFields);
  TEST_ASSERT_TRUE(static_cast<bool>(topic));
  TEST_ASSERT_TRUE(topic.log(Sample{3000, 1.0f}));

  size_t first_size = 0;
  const uint8_t* first = spool.rotate(&first_size);
  TEST_ASSERT_TRUE(first_size > 0);

  // logging continues into the second buffer, the first stays valid
  TEST_ASSERT_TRUE(topic.log(Sample{3001, 2.0f}));
  TEST_ASSERT_TRUE(spool.pending() > 0);

  std::vector<uint8_t> stream(first, first + first_size);
  Reader reader(stream);
  Msg msg;
  int data_count = 0;
  while (reader.next(msg)) {
    if (msg.type == 'D') ++data_count;
  }
  TEST_ASSERT_EQUAL_INT(1, data_count);  // not clobbered by post-rotate writes
}
