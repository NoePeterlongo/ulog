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
For the LZ4 sink, copy this repo's `lib/lz4/` as well. Alternatively reference
a git checkout:

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
| `task_stack_bytes` | 4096 | Drain task stack. |
| `task_priority` | 3 | Drain task priority. |
| `task_core` | `tskNO_AFFINITY` | 0 or 1 to pin the drain task (e.g. 0, since Arduino `loop()` runs on core 1). |

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
