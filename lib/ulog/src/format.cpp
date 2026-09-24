#include "ulog/format.hpp"

#include <cstring>

namespace ulog {

namespace {

bool is_field_name_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

bool is_space(char c) { return c == ' ' || c == '\t'; }

}  // namespace

uint16_t basic_type_size(const char* type, size_t len) {
  struct Entry {
    const char* name;
    uint16_t size;
  };
  static const Entry kTypes[] = {
      {"bool", 1},   {"char", 1},    {"int8_t", 1},  {"uint8_t", 1},
      {"int16_t", 2}, {"uint16_t", 2}, {"int32_t", 4}, {"uint32_t", 4},
      {"float", 4},  {"int64_t", 8}, {"uint64_t", 8}, {"double", 8},
  };
  for (const Entry& e : kTypes) {
    if (std::strlen(e.name) == len && std::strncmp(e.name, type, len) == 0) {
      return e.size;
    }
  }
  return 0;
}

bool for_each_field(const char* fields, FieldVisitor visitor, void* ctx) {
  const char* p = fields;
  while (*p != '\0') {
    while (is_space(*p)) ++p;
    if (*p == '\0') break;

    const char* type = p;
    size_t type_len = 0;
    while (*p != '\0' && !is_space(*p) && *p != '[' && *p != ';') {
      ++p;
      ++type_len;
    }
    if (type_len == 0) return false;

    size_t array_len = 1;
    if (*p == '[') {
      ++p;
      array_len = 0;
      while (*p >= '0' && *p <= '9') {
        array_len = array_len * 10 + static_cast<size_t>(*p - '0');
        if (array_len > 100000) return false;
        ++p;
      }
      if (*p != ']' || array_len == 0) return false;
      ++p;
    }
    while (is_space(*p)) ++p;

    const char* name = p;
    size_t name_len = 0;
    while (*p != '\0' && *p != ';') {
      if (!is_field_name_char(*p)) return false;
      ++p;
      ++name_len;
    }
    if (name_len == 0) return false;
    if (*p == ';') ++p;

    Field field{type, type_len, array_len, name, name_len};
    if (!visitor(field, ctx)) return false;
  }
  return true;
}

}  // namespace ulog
