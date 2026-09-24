#include <unity.h>

#include "test_helpers.hpp"

#include "ulog/format.hpp"

using ulog::Field;
using ulog::for_each_field;

namespace {

struct Capture {
  std::vector<Field> fields;
  bool ok = true;
};

bool capture(const Field& field, void* ctx) {
  static_cast<Capture*>(ctx)->fields.push_back(field);
  return true;
}

bool abort_visit(const Field&, void*) { return false; }

bool field_is(const Field& f, const char* type, size_t array_len, const char* name) {
  return strlen(type) == f.type_len && strncmp(type, f.type, f.type_len) == 0 &&
         f.array_len == array_len && strlen(name) == f.name_len &&
         strncmp(name, f.name, f.name_len) == 0;
}

}  // namespace

void test_for_each_field_basics() {
  Capture c;
  TEST_ASSERT_TRUE(for_each_field("uint64_t timestamp;float[4] q;", capture, &c));
  TEST_ASSERT_EQUAL_UINT32(2, c.fields.size());
  TEST_ASSERT_TRUE(field_is(c.fields[0], "uint64_t", 1, "timestamp"));
  TEST_ASSERT_TRUE(field_is(c.fields[1], "float", 4, "q"));

  // tolerated: missing trailing ';', extra spaces
  Capture c2;
  TEST_ASSERT_TRUE(for_each_field(" uint8_t a ; float[2] b;", capture, &c2));
  TEST_ASSERT_EQUAL_UINT32(2, c2.fields.size());
  TEST_ASSERT_TRUE(field_is(c2.fields[0], "uint8_t", 1, "a"));
  TEST_ASSERT_TRUE(field_is(c2.fields[1], "float", 2, "b"));

  // empty string parses but produces no fields (rejected later by declare)
  Capture c3;
  TEST_ASSERT_TRUE(for_each_field("", capture, &c3));
  TEST_ASSERT_EQUAL_UINT32(0, c3.fields.size());

  // visitor returning false aborts the walk
  Capture c4;
  TEST_ASSERT_FALSE(for_each_field("uint8_t a;uint8_t b;", abort_visit, &c4));
  TEST_ASSERT_EQUAL_UINT32(0, c4.fields.size());
}

void test_for_each_field_rejects() {
  Capture c;
  TEST_ASSERT_FALSE(for_each_field("uint8_t;", capture, &c));      // no name
  TEST_ASSERT_FALSE(for_each_field("float[4];", capture, &c));     // array, no name
  TEST_ASSERT_FALSE(for_each_field("[4] x;", capture, &c));        // no type
  TEST_ASSERT_FALSE(for_each_field("float;", capture, &c));        // no name
  TEST_ASSERT_FALSE(for_each_field("float[0] x;", capture, &c));   // zero array
  TEST_ASSERT_FALSE(for_each_field("float[100001] x;", capture, &c));  // array too big
  TEST_ASSERT_FALSE(for_each_field("float x-y;", capture, &c));    // bad name char
  TEST_ASSERT_FALSE(for_each_field(";", capture, &c));             // no field
  TEST_ASSERT_FALSE(for_each_field("uint8_t a b;", capture, &c));  // space in name
  TEST_ASSERT_FALSE(for_each_field("uint8_t a;;", capture, &c));   // empty second field
}
