#pragma once

// Portable ULog stream summarizer: walks raw bytes and produces human-readable
// lines plus counters. Used by LoggerBase::debug_dump() to print the log over
// the serial console on demand. Read-only; buffers at most a small prefix of
// each message, so it can summarize arbitrarily large streams chunk by chunk.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace ulog {

struct LogSummary {
  size_t bytes = 0;
  uint16_t formats = 0;
  uint16_t subscriptions = 0;
  uint32_t samples = 0;
  uint32_t infos = 0;
  uint32_t params = 0;
  uint32_t texts = 0;
  uint32_t syncs = 0;
  uint32_t dropout_ms = 0;
  bool truncated = false;  // stream ends in the middle of a message
};

class UlogSummary {
 public:
  UlogSummary(void (*emit)(void* ctx, const char* line), void* ctx)
      : emit_(emit), ctx_(ctx) {}

  // Feeds raw stream bytes (file header excluded) in arbitrary chunks.
  void feed(const uint8_t* data, size_t len) {
    stats_.bytes += len;
    while (len > 0) {
      if (state_ == kHeader) {
        const size_t need = 3 - got_;
        const size_t take = need < len ? need : len;
        memcpy(header_ + got_, data, take);
        got_ += take;
        data += take;
        len -= take;
        if (got_ == 3) {
          size_ = header_[0] | (header_[1] << 8);
          type_ = header_[2];
          consumed_ = 0;
          captured_ = 0;
          state_ = kPayload;
        }
      } else {
        const size_t remaining = size_ - consumed_;
        const size_t take = remaining < len ? remaining : len;
        const size_t room = kMaxPayload - captured_;
        const size_t copy = take < room ? take : room;
        if (copy > 0) {
          memcpy(payload_ + captured_, data, copy);
          captured_ += copy;
        }
        consumed_ += take;
        data += take;
        len -= take;
        if (consumed_ == size_) {
          process();
          state_ = kHeader;
          got_ = 0;
        }
      }
    }
  }

  // Ends the walk; emits one "data: <name>: N samples" line per topic.
  LogSummary finish() {
    if (state_ != kHeader || got_ != 0) stats_.truncated = true;
    char line[64];
    for (uint16_t i = 0; i < topic_count_; ++i) {
      snprintf(line, sizeof(line), "data: %s: %u samples", topics_[i].name,
               topics_[i].count);
      emit_(ctx_, line);
    }
    if (unknown_samples_ > 0) {
      snprintf(line, sizeof(line), "data: (unknown id): %u samples",
               unknown_samples_);
      emit_(ctx_, line);
    }
    return stats_;
  }

 private:
  static constexpr size_t kMaxPayload = 96;
  static constexpr size_t kMaxTopics = 64;

  void process() {
    const uint8_t* p = payload_;
    char line[128];

    switch (type_) {
      case 'B':
      case 'S':
        if (type_ == 'S') ++stats_.syncs;
        break;
      case 'F': {  // "name:fields"
        size_t colon = 0;
        while (colon < captured_ && p[colon] != ':') ++colon;
        const size_t name_len = colon < 22 ? colon : 22;
        memcpy(line, "format: ", 8);
        memcpy(line + 8, p, name_len);
        line[8 + name_len] = '\0';
        emit_(ctx_, line);
        ++stats_.formats;
        break;
      }
      case 'A': {  // multi_id, msg_id, name
        if (size_ < 3 || captured_ < 3) break;
        const uint16_t id = p[1] | (p[2] << 8);
        if (topic_count_ >= kMaxTopics) break;
        topics_[topic_count_].id = id;
        size_t name_len = size_ - 3;
        const size_t room = captured_ - 3;
        if (name_len > room) name_len = room;
        if (name_len > 23) name_len = 23;
        memcpy(topics_[topic_count_].name, p + 3, name_len);
        topics_[topic_count_].name[name_len] = '\0';
        snprintf(line, sizeof(line), "subscribe: %s (id %u)",
                 topics_[topic_count_].name, id);
        emit_(ctx_, line);
        ++topic_count_;
        ++stats_.subscriptions;
        break;
      }
      case 'D': {
        if (captured_ < 2) break;
        const uint16_t id = p[0] | (p[1] << 8);
        ++stats_.samples;
        for (uint16_t i = 0; i < topic_count_; ++i) {
          if (topics_[i].id == id) {
            ++topics_[i].count;
            return;
          }
        }
        ++unknown_samples_;
        break;
      }
      case 'I':
      case 'P': {  // key_len, "type name", value
        if (captured_ < 1) break;
        const uint8_t key_len = p[0];
        if (static_cast<size_t>(1 + key_len) > captured_) break;
        const char* key = reinterpret_cast<const char*>(p + 1);
        const uint8_t* value = p + 1 + key_len;
        size_t value_len = size_ - 1 - key_len;
        const size_t room = captured_ - 1 - key_len;
        if (value_len > room) value_len = room;
        char value_str[32];
        if (strncmp(key, "float ", 6) == 0 && value_len >= 4) {
          uint32_t raw = value[0] | (value[1] << 8) | (value[2] << 16) |
                         (static_cast<uint32_t>(value[3]) << 24);
          float f;
          memcpy(&f, &raw, 4);
          snprintf(value_str, sizeof(value_str), "%g", static_cast<double>(f));
        } else if (strncmp(key, "int32_t ", 8) == 0 && value_len >= 4) {
          snprintf(value_str, sizeof(value_str), "%d",
                   static_cast<int>(value[0] | (value[1] << 8) |
                                    (value[2] << 16) |
                                    (static_cast<int>(value[3]) << 24)));
        } else {
          const size_t n = value_len < 20 ? value_len : 20;
          memcpy(value_str, value, n);
          value_str[n] = '\0';
        }
        snprintf(line, sizeof(line), "%s: %.*s = %s",
                 type_ == 'I' ? "info" : "param", key_len, key, value_str);
        emit_(ctx_, line);
        if (type_ == 'I') ++stats_.infos; else ++stats_.params;
        break;
      }
      case 'L': {  // level, timestamp, text
        if (captured_ < 9) break;
        static const char* kLevels[8] = {"EMERG", "ALERT", "CRIT", "ERR",
                                         "WARN", "NOTICE", "INFO", "DEBUG"};
        const char* level_str =
            (p[0] >= '0' && p[0] <= '7') ? kLevels[p[0] - '0'] : "?";
        size_t text_len = size_ - 9;
        const size_t room = captured_ - 9;
        if (text_len > room) text_len = room;
        const size_t n = text_len < 60 ? text_len : 60;
        snprintf(line, sizeof(line), "text[%s]: %.*s", level_str,
                 static_cast<int>(n), p + 9);
        emit_(ctx_, line);
        ++stats_.texts;
        break;
      }
      case 'O': {
        if (captured_ < 2) break;
        const uint32_t duration = p[0] | (p[1] << 8);
        stats_.dropout_ms += duration;
        snprintf(line, sizeof(line), "dropout: %u ms", duration);
        emit_(ctx_, line);
        break;
      }
      default:
        break;  // parsers must ignore unknown message types
    }
  }

  struct Topic {
    uint16_t id;
    uint32_t count;
    char name[24];
  };

  void (*emit_)(void* ctx, const char* line);
  void* ctx_;
  Topic topics_[kMaxTopics] = {};
  uint16_t topic_count_ = 0;
  uint32_t unknown_samples_ = 0;
  LogSummary stats_{};
  uint8_t header_[3] = {};
  uint8_t payload_[kMaxPayload] = {};
  size_t size_ = 0;
  size_t consumed_ = 0;
  size_t captured_ = 0;
  uint8_t type_ = 0;
  uint8_t got_ = 0;
  enum State { kHeader, kPayload } state_ = kHeader;
};

// Convenience wrapper for a full in-memory stream (file header included).
inline LogSummary summarize_ulog(const uint8_t* data, size_t len,
                                 void (*emit)(void* ctx, const char* line),
                                 void* ctx) {
  UlogSummary walker(emit, ctx);
  if (len >= 16) {
    walker.feed(data + 16, len - 16);
  }
  LogSummary summary = walker.finish();
  if (len < 16) summary.truncated = true;
  summary.bytes = len;
  return summary;
}

}  // namespace ulog
