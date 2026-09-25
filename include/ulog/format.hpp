#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ulog {

struct Field {
  const char* type;
  size_t type_len;
  size_t array_len;  // number of elements, 1 for scalars
  const char* name;
  size_t name_len;
};

// Visits each field of a ULog field list, e.g. "uint64_t timestamp;float[3] v;".
// The visitor returns false to abort with an error. Field strings point into
// the original string, which must outlive the call.
using FieldVisitor = bool (*)(const Field& field, void* ctx);

bool for_each_field(const char* fields, FieldVisitor visitor, void* ctx);

// Size in bytes of a basic type name ("float", "uint64_t", ...), 0 if unknown.
uint16_t basic_type_size(const char* type, size_t len);

}  // namespace ulog
