# ulog

A small C++17 library that writes [PX4 ULog](docs/PX4_ulog/ulog_file_format.md)
files from an embedded system (ESP32-S3 first target, host-testable
anywhere). The encoder core knows nothing about the storage medium: ram,
LittleFS, serial or a compressed file are all just sinks. Logs produced by
this library parse with the standard toolchain (pyulog, Plot Juggler,
Foxglove, MAVLink ulg streaming).

The repository is a PlatformIO **library repository**: the library lives at
the root (`include/` + `src/`), so it can be consumed from another project
in any of the standard ways:

```ini
# platformio.ini of the consuming project
[env:my-drone]
lib_deps = https://github.com/USER/ulog.git     # or a tag: ...git#v0.1.0
```

or as a git submodule:

```bash
git submodule add https://github.com/USER/ulog.git lib/ulog
```

or simply copy the repository into `lib/`. Only `ulog/lz4_sink.hpp` needs
the vendored lz4 (`lib/lz4/` in this repository): copy it into the
consumer's `lib/` if compression is wanted — everything else works
without it.

## Features

- Full ULog stream: header, flag bits `'B'`, formats `'F'`, subscriptions
  `'A'`, data `'D'`, text `'L'`, info `'I'`, parameters `'P'`, sync `'S'`,
  dropouts `'O'` (levels as ASCII digits, matching PX4 and pyulog)
- Zero blocking for producers: writes are a mutex + memcpy; a failed write
  only returns false; overflow drops whole messages and records the lost
  time as ULog dropouts
- Message-atomic buffers everywhere: a torn write never corrupts the
  stream, parsers always see whole messages
- Two usage profiles: **streaming** (ring buffer + drain task, for slow
  continuous sinks) and **spool** (RAM while flying, burst to flash when
  landed — the drone black-box pattern)
- `ulog::LoggerBase` facade: one file per boot, `persist()` on landing,
  `erase_flash()`, `restart()`, serial `debug_dump()`
- Optional LZ4 compression producing standard `.lz4` files
- No dynamic allocation in the encoder; fixed-capacity format registry

## Quick start

### Flight recorder (recommended for drones)

```cpp
#include "ulog/logger.hpp"

class MyLogger : public ulog::LoggerBase {
 public:
  MyLogger() : LoggerBase("/log%03d.ulg") {}  // one file per boot, %03d
  void log_imu(uint64_t ts, const float gyro[3]) { imu_.log(Imu{ts, {gyro[0], gyro[1], gyro[2]}}); }
 protected:
  bool on_declare() override {
    imu_ = declare("sensor_imu", "uint64_t timestamp;float[3] gyro_rad;");
    return static_cast<bool>(imu_);  // false = boot error
  }
 private:
  struct __attribute__((packed)) Imu { uint64_t timestamp; float gyro_rad[3]; };
  ulog::Message imu_;
};

MyLogger logger;
logger.begin();          // reserves the spool (PSRAM), declares messages
logger.log_imu(ts, g);   // any task, RAM only, never blocks, no flash stall
logger.persist();        // landed: append everything to the file
logger.end();            // persist + close
logger.debug_dump(Serial);  // human summary of the whole log on demand
```

Measured on hardware: 200 Hz logging with zero flash stalls while flying,
zero dropouts; everything else in this README builds on the same
`Writer`/`Sink` pieces.

### Lower-level API

```cpp
ulog::FileSink sink("/tmp/log.ulg");     // or RamSink, LittleFsSink, ...
ulog::Writer logger{sink};
auto imu = logger.declare("sensor_imu",
                          "uint64_t timestamp;float[3] gyro_rad;");
imu.log(sample);                          // packed struct, memcpy'd out
logger.log_text(ulog::Level::Info, "hello");
logger.add_info("ver_hw", "my-board");
logger.add_param("pid_kp", 1.5f);
```

Payload structs must be trivially copyable and packed to match the format
string. `declare()` must run before the data section starts (it is
rejected once any data has been logged). See the Options section below and
the headers for the streaming profile (`ulog/async.hpp`) and compression
(`ulog/lz4_sink.hpp`).

## Repository layout

```
include/ulog/          public headers
  writer.hpp           Writer, Message, WriterTarget/WriterLock, Level
  logger.hpp           LoggerBase flight-recorder facade (ESP32+Arduino)
  log_summary.hpp      portable ULog walker behind debug_dump()
  sink.hpp             Sink interface
  ram_sink.hpp / file_sink.hpp          host sinks
  littlefs_sink.hpp    ESP32 LittleFS sink (size cap, append mode)
  spool_sink.hpp       RAM spool, double-buffer rotate (drone pattern)
  ring.hpp + async.hpp                  streaming profile (ESP32)
  freertos_lock.hpp    FreeRtosLock
  lz4_sink.hpp         LZ4 frame wrapper (needs lib/lz4)
  format.hpp           ULog field-list parser
src/                   encoder + parser implementation
lib/lz4/               vendored lz4 1.9.4 (BSD-2); optional, for Lz4Sink
examples/              buildable examples (see below)
test/                  Unity tests (native) + pyulog validation
tools/pull_littlefs.py extract LittleFS files from a device with esptool
docs/PX4_ulog/         PX4 ULog format documentation
```

## Examples

Each native example is a single `main.cpp` with the exact `g++` command at
the top — no PlatformIO needed, run from the repository root:

| Example | Shows | Command (from the repo root) |
|---|---|---|
| `native_minimal` | declare one topic, log samples, write a `.ulg` | `g++ -std=gnu++17 -Wall -Iinclude examples/native_minimal/main.cpp src/writer.cpp src/format.cpp -o /tmp/minimal && /tmp/minimal` |
| `native_spool_flight` | the flight-recorder pattern: `SpoolSink` in RAM while flying, burst-append on landing, `UlogSummary` printout (what `debug_dump()` shows on serial) | `g++ -std=gnu++17 -Wall -Iinclude examples/native_spool_flight/main.cpp src/writer.cpp src/format.cpp -o /tmp/spool && /tmp/spool` |
| `native_lz4` | optional compression into a standard `.lz4` file (needs `lib/lz4/`) | `g++ -std=gnu++17 -Wall -Iinclude -Ilib/lz4/src examples/native_lz4/main.cpp src/writer.cpp src/format.cpp lib/lz4/src/lz4.c lib/lz4/src/lz4hc.c lib/lz4/src/lz4frame.c lib/lz4/src/xxhash.c -o /tmp/lz4demo && /tmp/lz4demo` |
| `esp32_flight_recorder` | the `LoggerBase` facade on an ESP32-S3: two flights per boot into one file, debug dump over serial | `pio run -d examples/esp32_flight_recorder -t upload` |

All example outputs parse with pyulog; try `pip install pyulog` and
`pyulog info /tmp/ulog_minimal.ulg`.

## Options

### `ulog::Writer::Config`

| Option | Default | Meaning |
|---|---|---|
| `now_us` | `default_clock_us` | Clock in µs (`esp_timer_get_time()` on ESP32, `steady_clock` on host). Used for the file header and `'L'`/`'O'` timestamps. |
| `sync_interval_bytes` | 4096 | Emit a `'S'` sync message every N encoded bytes so parsers can resync after corruption. 0 disables. |

### `ulog::LoggerBase` (facade)

`begin()` / `persist()` / `end()` / `erase_flash()` (formats the partition,
seconds, ground only) / `restart()` (fresh stream, `on_declare()` re-runs,
handles refreshed) / `debug_dump(Print&)` / `pending()`, `spool_near_full()`
(persist early if a stall is acceptable), `persisted_bytes()`, `dropped_ms()`,
`session_path()`. Text logging is public and callable from any task:
`log_text(Level, const char*)` plus `log_info()` / `log_warning()` /
`log_error()` shortcuts; they return false outside a session.
`LoggerBase::Config`: `spool_capacity` (default 2 MB,
PSRAM, doubled internally for the two spool buffers).

### `ulog::AsyncWriter::Config` (streaming profile)

| Option | Default | Meaning |
|---|---|---|
| `ring_storage` / `ring_size` | – / 0 | Backing memory for the ring, PSRAM recommended. `start()` fails if unset. |
| `drain_chunk` | 1024 | Max bytes written to the sink per drain iteration. |
| `flush_interval_ms` | 100 | Max time data may wait in the ring. Higher = fewer flash operations (each LittleFS erase stalls both cores ~45 ms); 0 = ASAP. |
| `task_stack_bytes` | 4096 | Drain task stack. |
| `task_priority` | 3 | Drain task priority. |
| `task_core` | `tskNO_AFFINITY` | 0 or 1 to pin the drain task. |

### `ulog::Lz4Sink::Config`

| Option | Default | Meaning |
|---|---|---|
| `independent_blocks` | `true` | Independent 64 KB blocks: better corruption isolation, slightly lower ratio. |
| `content_checksum` | `false` | XXH64 of the whole content in the footer. |
| `compression_level` | 0 | 0 = fast LZ4; up to 16 uses LZ4HC. |

### `ulog::LittleFsSink`

`LittleFsSink(path, max_bytes = 0, format_on_fail = true)`,
`open(append = false)`.

### Limits

- 32 message formats max, names up to 23 chars, payload ≤ 64 KB (ULog
  16-bit message size), `'I'` string values ≤ 192 bytes
- No dynamic allocation in the encoder; heap uses are the facade spool
  buffers, the `AsyncWriter` drain chunk and the `Lz4Sink` buffers

### Thread-safety

`Writer` serializes every public call when a `WriterLock` is provided
(`LoggerBase` and `AsyncWriter` install a FreeRTOS mutex). Without a lock,
single-task use only. `Message` handles and the logger pointer can be
shared in an application context once set; re-declare after `restart()`.

## Getting logs off the device

```
python3 tools/pull_littlefs.py --port /dev/ttyUSB0 --out logs/
python3 tools/pull_littlefs.py --image littlefs.bin --out logs/   # existing dump
lz4 -d log001.ulg.lz4 log001.ulg                                  # if compressed
```

It auto-detects the LittleFS partition from the partition table
(`--label spiffs` to pick one explicitly). Requires `esptool` and
`pip install littlefs-python`.

## Testing

Everything except the ESP32-only headers runs on the host:

```
pio test -e native                       # 40 Unity tests
python3 test/pyulog_check.py              # sync round-trip, strict expectations
python3 test/pyulog_check.py --profile async   # ring drops -> 'O' messages
python3 test/pyulog_check.py --profile lz4     # LZ4 round-trip + crash-prefix recovery
pio run -d examples/esp32_flight_recorder -t upload   # flash the demo
```

The ESP32-S3 example runs two flights per boot into one file and prints a
summary over serial (`d` on the console for the debug dump). Requires
`pyulog` and `lz4` (python bindings) for the validation script.

## Design notes and limits

- Flash erases stall both cores (cache suspend, ~45 ms per LittleFS
  commit on this hardware): the flight-recorder profile touches flash
  only at `persist()`, the streaming profile batches via
  `flush_interval_ms`.
- If the sink fails persistently (disk full), data is dropped without a
  dropout marker in the streaming profile; the facade reports it via
  `persist()`'s return value.
- Crash data appending (`incompat_flags` + `appended_offsets`) is not
  implemented.
