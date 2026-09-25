# ulog

A small C++17 library that writes [PX4 ULog](https://mavlink.io/en/log/ulog_file_format.html)
files from an embedded system. The encoder is media-agnostic: ram, LittleFS,
serial or a compressed file are all just sinks. Produced logs parse with the
standard toolchain (pyulog, Plot Juggler, Foxglove).

Core features: zero-blocking producers with dropout reporting, message-atomic
ring buffer (a torn write never corrupts the stream), nested formats, optional
LZ4 compression producing standard `.lz4` files.

## Install

PlatformIO (recommended): copy `lib/ulog/` into your project's `lib/`
directory — that is all; the library is picked up by the dependency scanner.
Only `ulog/lz4_sink.hpp` needs the vendored lz4: copy this repo's `lib/lz4/`
too, or skip it — **everything else works without it** (lz4 is linked only
when you actually include `lz4_sink.hpp`; including it without the library
fails with an explicit `#error`). Alternatively reference a git checkout:

```ini
# platformio.ini
[env:esp32-s3]
platform = ...
lib_deps =
    ulog @ symlink://<path to this repo>/lib/ulog
    lz4 @ symlink://<path to this repo>/lib/lz4   # optional, for Lz4Sink
```

No other dependencies. Requirements: C++17. Everything works on any host;
`async.hpp` and `littlefs_sink.hpp` are ESP32/FreeRTOS only.

## Usage

### Synchronous writer (single task)

```cpp
#include "ulog/file_sink.hpp"
#include "ulog/writer.hpp"

ulog::FileSink sink("/tmp/log.ulg");   // or RamSink, LittleFsSink, ...
ulog::Writer logger{sink};

struct __attribute__((packed)) Imu {
  uint64_t timestamp;
  float gyro_rad[3];
};

// the format string is the ULog 'F' message, verbatim
auto imu = logger.declare("sensor_imu",
                          "uint64_t timestamp;float[3] gyro_rad;");
if (imu) imu.log(Imu{esp_timer_get_time(), {0.1f, 0.f, 0.f}});

logger.log_text(ulog::Level::Info, "boot ok");
logger.add_info("ver_hw", "my-board");       // 'I' messages
logger.add_param("pid_kp", 1.5f);             // 'P' messages (int32_t/float)
```

Payload structs must be trivially copyable and packed to match the format
string (`__attribute__((packed))` is mandatory unless `uint64_t timestamp` is
the first member). `Message::log()` returns `false` on rejection, overflow
or sink failure; timestamps must not decrease per message.

### Asynchronous writer (ESP32, FreeRTOS)

Producers only copy into a ring buffer and never block. A drain task writes
to the sink; overflows drop whole messages and are reported with `'O'`
dropout messages carrying the lost duration.

```cpp
#include "ulog/async.hpp"
#include "ulog/littlefs_sink.hpp"

ulog::LittleFsSink flash{"/log001.ulg"};
flash.open();

ulog::AsyncWriter::Config cfg;
cfg.ring_storage = heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM);
cfg.ring_size = 64 * 1024;

ulog::AsyncWriter logger{flash, ulog::Writer::Config(), cfg};
logger.start();
auto imu = logger.declare("sensor_imu", "uint64_t timestamp;float[3] gyro_rad;");
imu.log(sample);   // microseconds, never blocks

// when done, stop producers first:
logger.stop();
flash.close();
```

### Compression

`Lz4Sink` wraps any sink and produces a standard LZ4 frame. The file is
`log001.ulg.lz4`, not a `.ulg` — the ULog spec has no in-format compression:

```cpp
#include "ulog/lz4_sink.hpp"

ulog::LittleFsSink flash{"/log001.ulg.lz4"};
flash.open();
ulog::Lz4Sink sink{flash};              // drop-in between logger and sink
...
sink.finish();                          // frame footer, once at the end
```

On the host: `lz4 -d log001.ulg.lz4 log001.ulg`, then any ULog tool.
Typical ratio 2-4x on sensor data. Data flushed by `sync()` (called
automatically when the drain task goes idle) survives a crash; the tail
since the last flush is lost. Blocks are independent, so one corrupted
64 KB block does not compromise the following ones. Costs about 130 KB
of internal heap.

### Log to RAM while flying, flush to flash when safe

Every littlefs write/erase suspends the flash cache and stalls code on
both cores for tens of ms. If other tasks have hard timing requirements
(control loops, state machines), don't touch flash while they run.

Two flavors:

**Streaming** (`RingBuffer` + `AsyncWriter` + `LittleFsSink`): logs are
written to flash continuously through a ring buffer; overflow drops whole
messages and reports them as `'O'` dropouts. For continuous, long-running
logs where a small loss is acceptable.

**Spool** (`SpoolSink`): the whole session stays in RAM in two
pre-reserved buffers (no allocation in flight); `rotate()` hands the
spooled bytes to the flash writer in O(1) while logging continues in the
second buffer. When the spool is full, whole messages are dropped with
dropout accounting — never an allocation failure, never a torn message.
Capacity is the design margin: `>= rate * time between two persist()`.

See the project root for the `LoggerBase` facade that packages the spool
pattern (one file per boot, `persist()` on landing, `erase_flash()`,
`restart()`, serial `debug_dump()`).

## Flight recorder facade (`ulog/logger.hpp`)

`ulog::LoggerBase` is the packaged drone pattern: one ULog stream per boot
in RAM (`SpoolSink`), appended to a single LittleFS file at each
`persist()` — call it when landed. Derive it per project:

```cpp
#include "ulog/logger.hpp"

class MyLogger : public ulog::LoggerBase {
 public:
  MyLogger() : LoggerBase("/log%03d.ulg") {}  // one file per boot

  void log_imu(uint64_t ts, const float gyro[3], const float accel[3]) {
    imu_.log(ImuSample{ts, {gyro[0], gyro[1], gyro[2]},
                       {accel[0], accel[1], accel[2]}});
  }

 protected:
  bool on_declare() override {   // runs after begin() and restart()
    imu_ = declare("sensor_imu",
                   "uint64_t timestamp;float[3] gyro_rad;float[3] accel_mps2;");
    return static_cast<bool>(imu_);  // false = boot error
  }

 private:
  struct __attribute__((packed)) ImuSample {
    uint64_t timestamp;
    float gyro_rad[3];
    float accel_mps2[3];
  };
  ulog::Message imu_;
};
```

API: `begin()`, `persist()` (returns bytes written), `end()`,
`erase_flash()` (formats the partition, seconds, ground only),
`restart()` (fresh stream, `on_declare()` re-runs, handles refreshed),
`debug_dump(Print&)` (human summary of file + spool over e.g. Serial),
`pending()`, `spool_near_full()` (persist early if a stall is
acceptable), `persisted_bytes()`, `dropped_ms()`, `session_path()`.

`LoggerBase::Config`: `spool_capacity` (default 2 MB, PSRAM, doubled
internally for the two spool buffers). Requires Arduino + LittleFS.

## Options

### `ulog::Writer::Config`

| Option | Default | Meaning |
|---|---|---|
| `now_us` | `default_clock_us` | Clock in µs (`esp_timer_get_time()` on ESP32, `steady_clock` on host). Used for the file header, `'L'`/`'O'` timestamps. |
| `sync_interval_bytes` | 4096 | Emit a `'S'` sync message every N encoded bytes so parsers can resync after corruption. 0 disables. |

### `ulog::AsyncWriter::Config`

| Option | Default | Meaning |
|---|---|---|
| `ring_storage` / `ring_size` | – / 0 | Backing memory for the ring, PSRAM recommended. `start()` fails if unset. |
| `drain_chunk` | 1024 | Max bytes written to the sink per drain iteration. |
| `flush_interval_ms` | 100 | Max time data may wait in the ring before being written. Higher = fewer flash operations on LittleFS (each erase stalls both cores); 0 = write/flush ASAP. |
| `task_stack_bytes` | 4096 | Drain task stack. |
| `task_priority` | 3 | Drain task priority. |
| `task_core` | `tskNO_AFFINITY` | 0 or 1 to pin the drain task (e.g. 0, since Arduino `loop()` runs on core 1). |

### `ulog::SpoolSink`

`SpoolSink(buffer_a, buffer_b, capacity, now_us)`: two caller-allocated
buffers (e.g. PSRAM, checked at boot), nothing allocated afterwards.
`pending()`, `near_full(watermark)`, `rotate(&size)` (O(1) handover to a
slow consumer), `take_dropped_ms()`. All methods must be serialized
externally — share the `Writer`'s `WriterLock` (`FreeRtosLock`).

### `ulog::Lz4Sink::Config`

| Option | Default | Meaning |
|---|---|---|
| `independent_blocks` | `true` | Independent 64 KB blocks: better corruption isolation, slightly lower ratio. |
| `content_checksum` | `false` | XXH64 over the whole content in the footer; detects corruption, costs a little CPU. |
| `compression_level` | 0 | 0 = fast LZ4; up to 16 uses LZ4HC (slower, tighter). |

### `ulog::LittleFsSink`

`LittleFsSink(path, max_bytes = 0, format_on_fail = true)`: `max_bytes`
refuses writes past the cap (the drain task then records dropouts);
`format_on_fail` formats the partition on first mount failure.

### Limits

- 32 message formats max, names up to 23 chars, payload ≤ 64 KB (ULog
  16-bit message size)
- `'I'` string values ≤ 192 bytes
- No dynamic allocation in the encoder; the only heap uses are the
  `AsyncWriter` drain chunk and the `Lz4Sink` buffers

### Thread-safety

`Writer` takes an optional `WriterLock` (`AsyncWriter` installs a FreeRTOS
mutex); every public call is serialized. Without a lock, single-task use
only. The ring is SPSC: multiple producers are serialized by the lock, one
drain task consumes.

## Testing

The library is host-testable except the two ESP32 headers:

```
pio test -e native                      # Unity tests
python3 test/pyulog_check.py             # pyulog validation (sync/async/lz4)
```

See the project root README for details.
