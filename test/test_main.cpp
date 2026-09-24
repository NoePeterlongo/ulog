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
void test_lz4_large_multiblock();
void test_lz4_options();
void test_lz4_inner_failure();
void test_lz4_crash_prefix();
void test_file_sink_failure();
void test_for_each_field_basics();
void test_for_each_field_rejects();
void test_info_numeric_types();
void test_add_info_limits();
void test_declare_limits();
void test_log_text_empty();
void test_writer_reports_sink_failure();
void test_ring_zero_capacity();
void test_ring_empty_messages();

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
  RUN_TEST(test_lz4_large_multiblock);
  RUN_TEST(test_lz4_options);
  RUN_TEST(test_lz4_inner_failure);
  RUN_TEST(test_lz4_crash_prefix);
  RUN_TEST(test_file_sink_failure);
  RUN_TEST(test_for_each_field_basics);
  RUN_TEST(test_for_each_field_rejects);
  RUN_TEST(test_info_numeric_types);
  RUN_TEST(test_add_info_limits);
  RUN_TEST(test_declare_limits);
  RUN_TEST(test_log_text_empty);
  RUN_TEST(test_writer_reports_sink_failure);
  RUN_TEST(test_ring_zero_capacity);
  RUN_TEST(test_ring_empty_messages);
  return UNITY_END();
}
