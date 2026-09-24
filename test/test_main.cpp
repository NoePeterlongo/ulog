#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

// Tests live in test_ulog.cpp.
void test_header_and_flag_bits();
void test_declare_format_message();
void test_declare_rejections();
void test_size_computation_and_padding();
void test_nested_format();
void test_log_data_and_subscription();
void test_timestamp_not_first_field();
void test_monotonic_timestamps();
void test_log_text();
void test_info_and_params();
void test_sync_messages();
void test_roundtrip_file();
void test_ring_write_read_wrap();
void test_ring_message_rollback();
void test_writer_ring_atomic_drops();
void test_writer_log_dropout();
void test_async_roundtrip_file();
void test_lz4_frame();
void test_lz4_roundtrip_file();

int main(int argc, char **argv) {
  UNITY_BEGIN();
  RUN_TEST(test_header_and_flag_bits);
  RUN_TEST(test_declare_format_message);
  RUN_TEST(test_declare_rejections);
  RUN_TEST(test_size_computation_and_padding);
  RUN_TEST(test_nested_format);
  RUN_TEST(test_log_data_and_subscription);
  RUN_TEST(test_timestamp_not_first_field);
  RUN_TEST(test_monotonic_timestamps);
  RUN_TEST(test_log_text);
  RUN_TEST(test_info_and_params);
  RUN_TEST(test_sync_messages);
  RUN_TEST(test_roundtrip_file);
  RUN_TEST(test_ring_write_read_wrap);
  RUN_TEST(test_ring_message_rollback);
  RUN_TEST(test_writer_ring_atomic_drops);
  RUN_TEST(test_writer_log_dropout);
  RUN_TEST(test_async_roundtrip_file);
  RUN_TEST(test_lz4_frame);
  RUN_TEST(test_lz4_roundtrip_file);
  return UNITY_END();
}
