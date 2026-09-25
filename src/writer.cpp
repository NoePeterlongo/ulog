#include "ulog/writer.hpp"

#include <stdio.h>
#include <string.h>

#include "ulog/format.hpp"

#if defined(ESP_PLATFORM)
#include <esp_timer.h>
#else
#include <chrono>
#endif

namespace ulog {

uint64_t default_clock_us() {
#if defined(ESP_PLATFORM)
  return esp_timer_get_time();
#else
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now).count());
#endif
}

namespace {

constexpr size_t kMaxInfoValue = 192;
constexpr size_t kMaxTypeNameLen = 12;  // "uint64_t", "char[255]"

const uint8_t kFileMagic[7] = {0x55, 0x4c, 0x6f, 0x67, 0x01, 0x12, 0x35};
const uint8_t kSyncMagic[8] = {0x2F, 0x73, 0x13, 0x20, 0x25, 0x0C, 0xBB, 0x12};

void put_le16(uint8_t* out, uint16_t v) {
  out[0] = static_cast<uint8_t>(v);
  out[1] = static_cast<uint8_t>(v >> 8);
}

void put_le64(uint8_t* out, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out[i] = static_cast<uint8_t>(v >> (8 * i));
  }
}

uint64_t load_le64(const uint8_t* in) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(in[i]) << (8 * i);
  }
  return v;
}

bool is_key_name_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '/';
}

bool valid_key_name(const char* name, size_t len) {
  if (len == 0) return false;
  for (size_t i = 0; i < len; ++i) {
    if (!is_key_name_char(name[i])) return false;
  }
  return true;
}

bool is_padding(const char* name, size_t len) {
  return len >= 8 && strncmp(name, "_padding", 8) == 0;
}

class OptionalLock {
 public:
  explicit OptionalLock(WriterLock* lock) : lock_(lock) {
    if (lock_ != nullptr) lock_->lock();
  }
  ~OptionalLock() {
    if (lock_ != nullptr) lock_->unlock();
  }
  OptionalLock(const OptionalLock&) = delete;
  OptionalLock& operator=(const OptionalLock&) = delete;

 private:
  WriterLock* lock_;
};

}  // namespace

struct DeclareCtx {
  Writer* writer;
  uint32_t full_size = 0;
  uint32_t logged_size = 0;
  uint16_t ts_offset = Writer::kNoTimestamp;
  bool ok = true;

  bool visit(const Field& field) {
    uint16_t size = basic_type_size(field.type, field.type_len);
    if (size == 0) {
      size = writer->nested_type_size(field.type, field.type_len);
    }
    if (size == 0) {
      ok = false;
      return false;
    }

    if (ts_offset == Writer::kNoTimestamp && field.array_len == 1 &&
        field.name_len == 9 && strncmp(field.name, "timestamp", 9) == 0 &&
        field.type_len == 8 && strncmp(field.type, "uint64_t", 8) == 0) {
      ts_offset = static_cast<uint16_t>(full_size);
    }

    full_size += size * static_cast<uint32_t>(field.array_len);
    if (!is_padding(field.name, field.name_len)) {
      logged_size = full_size;
    }
    return true;
  }
};

namespace {

bool declare_visit(const Field& field, void* ctx) {
  return static_cast<DeclareCtx*>(ctx)->visit(field);
}

}  // namespace

Writer::Writer(Sink& sink) : Writer(sink, Config()) {}

Writer::Writer(Sink& sink, const Config& config)
    : direct_target_(sink),
      target_(&direct_target_),
      lock_(nullptr),
      now_us_(config.now_us),
      sync_interval_(config.sync_interval_bytes) {
  write_file_header();
}

Writer::Writer(WriterTarget& target, const Config& config, WriterLock* lock)
    : target_(&target),
      lock_(lock),
      now_us_(config.now_us),
      sync_interval_(config.sync_interval_bytes) {
  write_file_header();
}

void Writer::write_file_header() {
  begin_frame();
  uint8_t header[16];
  memcpy(header, kFileMagic, sizeof(kFileMagic));
  header[7] = 0x01;  // format version
  put_le64(header + 8, now_us_());
  end_frame(part(header, sizeof(header)));

  // 'B' flag bits: no compatibility features used, no appended data.
  begin_frame();
  uint8_t flags[40] = {};
  end_frame(begin_message('B', sizeof(flags)) && part(flags, sizeof(flags)));
}

void Writer::begin_frame() {
  msg_start_bytes_ = bytes_;
  target_->start_message();
}

bool Writer::end_frame(bool ok) {
  ok = target_->finish_message(ok);
  if (!ok) bytes_ = msg_start_bytes_;
  return ok;
}

bool Writer::begin_message(uint8_t type, size_t payload_size) {
  if (payload_size > 0xFFFF) return false;
  uint8_t header[3];
  put_le16(header, static_cast<uint16_t>(payload_size));
  header[2] = type;
  return part(header, sizeof(header));
}

bool Writer::part(const void* data, size_t size) {
  if (size == 0) return true;
  if (!target_->write(static_cast<const uint8_t*>(data), size)) return false;
  bytes_ += size;
  return true;
}

void Writer::maybe_sync() {
  if (sync_interval_ == 0 || bytes_ - last_sync_ < sync_interval_) return;
  begin_frame();
  bool ok =
      begin_message('S', sizeof(kSyncMagic)) && part(kSyncMagic, sizeof(kSyncMagic));
  if (end_frame(ok)) last_sync_ = bytes_;
}

uint16_t Writer::nested_type_size(const char* type, size_t len) {
  for (uint16_t i = 0; i < format_count_; ++i) {
    const char* name = formats_[i].name;
    if (strlen(name) == len && strncmp(name, type, len) == 0) {
      return formats_[i].full_size;
    }
  }
  return 0;
}

Message Writer::declare(const char* name, const char* fields) {
  OptionalLock lock(lock_);
  if (data_started_ || format_count_ >= kMaxFormats) return Message();

  size_t name_len = strlen(name);
  if (!valid_key_name(name, name_len) || name_len > kMaxNameLen ||
      basic_type_size(name, name_len) != 0) {
    return Message();
  }
  for (uint16_t i = 0; i < format_count_; ++i) {
    if (strcmp(formats_[i].name, name) == 0) return Message();
  }

  DeclareCtx ctx{this};
  if (!for_each_field(fields, declare_visit, &ctx) || !ctx.ok ||
      ctx.full_size == 0 || ctx.full_size > 0xFFFF) {
    return Message();
  }

  Format& format = formats_[format_count_];
  memcpy(format.name, name, name_len);
  format.full_size = static_cast<uint16_t>(ctx.full_size);
  format.logged_size = static_cast<uint16_t>(ctx.logged_size);
  format.ts_offset = ctx.ts_offset;
  format.msg_id = 0;
  format.subscribed = false;
  format.last_timestamp = 0;
  const uint8_t index = static_cast<uint8_t>(format_count_);
  ++format_count_;

  size_t fields_len = strlen(fields);
  bool add_semicolon = fields_len > 0 && fields[fields_len - 1] != ';';
  size_t payload_len = name_len + 1 + fields_len + (add_semicolon ? 1 : 0);
  begin_frame();
  bool ok = begin_message('F', payload_len) && part(name, name_len) &&
            part(":", 1) && part(fields, fields_len) &&
            (!add_semicolon || part(";", 1));
  if (!end_frame(ok)) {
    --format_count_;
    return Message();
  }
  return Message(*this, index, format.logged_size);
}

bool Writer::write_payload(uint8_t index, const void* payload) {
  OptionalLock lock(lock_);
  if (index >= format_count_) return false;
  Format& format = formats_[index];
  if (format.ts_offset == kNoTimestamp) return false;

  if (!format.subscribed) {
    const size_t name_len = strlen(format.name);
    uint8_t multi_id = 0;
    uint8_t id[2];
    put_le16(id, next_msg_id_);
    begin_frame();
    bool ok = begin_message('A', 3 + name_len) && part(&multi_id, 1) &&
              part(id, sizeof(id)) && part(format.name, name_len);
    if (!end_frame(ok)) return false;
    format.subscribed = true;
    data_started_ = true;
    format.msg_id = next_msg_id_++;
  }

  const uint8_t* bytes = static_cast<const uint8_t*>(payload);
  uint64_t timestamp = load_le64(bytes + format.ts_offset);
  if (timestamp < format.last_timestamp) return false;
  format.last_timestamp = timestamp;

  maybe_sync();

  uint8_t id[2];
  put_le16(id, format.msg_id);
  begin_frame();
  bool ok = begin_message('D', 2 + format.logged_size) &&
            part(id, sizeof(id)) && part(payload, format.logged_size);
  return end_frame(ok);
}

bool Writer::write_key_value(uint8_t type, const char* type_str,
                             const char* name, const void* value,
                             size_t value_size) {
  OptionalLock lock(lock_);
  size_t name_len = strlen(name);
  if (value_size > kMaxInfoValue || name_len > kMaxNameLen ||
      !valid_key_name(name, name_len)) {
    return false;
  }

  uint8_t buf[1 + kMaxTypeNameLen + 1 + kMaxNameLen + kMaxInfoValue];
  size_t pos = 0;
  buf[pos++] = 0;  // key_len, patched below
  size_t type_len = strlen(type_str);
  memcpy(buf + pos, type_str, type_len);
  pos += type_len;
  buf[pos++] = ' ';
  memcpy(buf + pos, name, name_len);
  pos += name_len;
  buf[0] = static_cast<uint8_t>(pos - 1);
  memcpy(buf + pos, value, value_size);
  pos += value_size;

  begin_frame();
  return end_frame(begin_message(type, pos) && part(buf, pos));
}

bool Writer::add_info(const char* name, const char* value) {
  size_t value_len = strlen(value);
  if (value_len == 0 || value_len > 255) return false;

  char type_str[kMaxTypeNameLen + 1];
  snprintf(type_str, sizeof(type_str), "char[%zu]", value_len);
  return write_key_value('I', type_str, name, value, value_len);
}

bool Writer::log_text(Level level, const char* text) {
  return log_text(level, text, strlen(text));
}

bool Writer::log_text(Level level, const char* text, size_t length) {
  OptionalLock lock(lock_);
  if (length > strlen(text)) return false;

  data_started_ = true;
  maybe_sync();
  uint8_t payload[9];
  payload[0] = static_cast<uint8_t>('0' + static_cast<unsigned>(level));
  put_le64(payload + 1, now_us_());
  begin_frame();
  bool ok = begin_message('L', 9 + length) && part(payload, sizeof(payload)) &&
            part(text, length);
  return end_frame(ok);
}

bool Writer::log_dropout(uint32_t duration_ms) {
  OptionalLock lock(lock_);
  data_started_ = true;
  while (duration_ms > 0) {
    const uint16_t chunk = duration_ms > 0xFFFF ? 0xFFFF : duration_ms;
    uint8_t payload[2];
    put_le16(payload, chunk);
    begin_frame();
    bool ok = begin_message('O', sizeof(payload)) && part(payload, sizeof(payload));
    if (!end_frame(ok)) return false;
    duration_ms -= chunk;
  }
  return true;
}

}  // namespace ulog
