#pragma once

// Sink wrapper compressing the log stream with the LZ4 frame format.
// The result is a standard .lz4 file ("log001.ulg.lz4"): decompress on the
// host with `lz4 -d` (or any LZ4-frame tool) before analyzing with pyulog,
// Plot Juggler, etc. Compression is transparent to the rest of the library:
// wrap any Sink, e.g. Lz4Sink(LittleFsSink(...)).
//
// Blocks are independent by default, so a corrupted 64 KB block does not
// compromise the following ones. Partial blocks stay buffered until sync()
// (idle flush) to keep the compression ratio up; call finish() once when
// logging is over to write the frame footer.

#if !__has_include(<lz4frame.h>)
#error "ulog/lz4_sink.hpp requires the lz4 library: vendor lib/lz4/ into your project, or drop this include (everything else in ulog works without it)"
#endif

#include <lz4frame.h>

#include <stdlib.h>

#include "ulog/sink.hpp"

namespace ulog {

class Lz4Sink final : public Sink {
 public:
  struct Config {
    bool independent_blocks = true;
    bool content_checksum = false;
    int compression_level = 0;  // 0 = default fast LZ4, up to LZ4HC levels
  };

  static constexpr size_t kBlockSize = 64 * 1024;  // smallest frame block size

  Lz4Sink(Sink& inner, const Config& config) : inner_(inner) {
    prefs_.frameInfo.blockMode =
        config.independent_blocks ? LZ4F_blockIndependent : LZ4F_blockLinked;
    prefs_.frameInfo.contentChecksumFlag = config.content_checksum
                                              ? LZ4F_contentChecksumEnabled
                                              : LZ4F_noContentChecksum;
    prefs_.compressionLevel = config.compression_level;
    dst_capacity_ = LZ4F_compressBound(kBlockSize, &prefs_);
    dst_ = static_cast<uint8_t*>(malloc(dst_capacity_));
    ok_ = dst_ != nullptr &&
          !LZ4F_isError(LZ4F_createCompressionContext(&ctx_, LZ4F_VERSION));
  }

  Lz4Sink(Sink& inner);  // defined below: uses Config defaults

  ~Lz4Sink() override {
    if (ctx_ != nullptr) LZ4F_freeCompressionContext(ctx_);
    free(dst_);
  }

  Lz4Sink(const Lz4Sink&) = delete;
  Lz4Sink& operator=(const Lz4Sink&) = delete;

  bool is_valid() const { return ok_; }

  bool write(const uint8_t* data, size_t size) override {
    if (size == 0) return true;
    if (!ok_ || finished_) return false;
    if (!begin_frame()) return false;
    while (size > 0) {
      const size_t chunk = size < kBlockSize ? size : kBlockSize;
      const size_t written = LZ4F_compressUpdate(ctx_, dst_, dst_capacity_,
                                                 data, chunk, nullptr);
      if (LZ4F_isError(written) ||
          (written > 0 && !inner_.write(dst_, written))) {
        ok_ = false;
        return false;
      }
      data += chunk;
      size -= chunk;
    }
    return true;
  }

  // Compresses out the buffered partial block, then syncs the inner sink.
  bool sync() override {
    if (!ok_) return false;
    if (frame_begun_ && !flush()) return false;
    return inner_.sync();
  }

  // Writes the frame footer. Call once after the last write.
  bool finish() {
    if (!ok_ || finished_) return false;
    if (!frame_begun_) {
      finished_ = true;
      return inner_.sync();
    }
    const size_t written = LZ4F_compressEnd(ctx_, dst_, dst_capacity_, nullptr);
    frame_begun_ = false;
    finished_ = true;
    if (LZ4F_isError(written) ||
        (written > 0 && !inner_.write(dst_, written))) {
      ok_ = false;
      return false;
    }
    return inner_.sync();
  }

 private:
  bool begin_frame() {
    if (frame_begun_) return true;
    const size_t written = LZ4F_compressBegin(ctx_, dst_, dst_capacity_, &prefs_);
    if (LZ4F_isError(written)) {
      ok_ = false;
      return false;
    }
    frame_begun_ = true;
    if (!inner_.write(dst_, written)) {
      ok_ = false;
      return false;
    }
    return true;
  }

  bool flush() {
    const size_t written = LZ4F_flush(ctx_, dst_, dst_capacity_, nullptr);
    if (LZ4F_isError(written)) {
      ok_ = false;
      return false;
    }
    return written == 0 || inner_.write(dst_, written);
  }

  Sink& inner_;
  LZ4F_preferences_t prefs_{};
  LZ4F_cctx* ctx_ = nullptr;
  uint8_t* dst_ = nullptr;
  size_t dst_capacity_ = 0;
  bool frame_begun_ = false;
  bool finished_ = false;
  bool ok_ = false;
};

inline Lz4Sink::Lz4Sink(Sink& inner) : Lz4Sink(inner, Config()) {}

}  // namespace ulog
