# ulog — media-agnostic ULog writer

A small C++17 library that writes [PX4 ULog](docs/PX4_ulog/ulog_file_format.md)
files from an embedded system (ESP32-S3 first target, host-testable anywhere).
The encoder core knows nothing about the storage medium: ram, LittleFS, serial
or a compressed file are all just sinks. Logs produced by this library parse
with the standard toolchain (pyulog, Plot Juggler, Foxglove, MAVLink ulg
streaming).

```
producer tasks ──► Writer ──► ring buffer ──► drain task ──► [Lz4Sink] ──► Sink
                 (encode,  (message-atomic,  (FreeRTOS)   (optional)  (LittleFS,
                  mutex)    drops = 'O')                                file, ram...)
```

## Features

- Full ULog stream: header, flag bits `'B'`, formats `'F'`, subscriptions
  `'A'`, data `'D'`, text `'L'`, info `'I'`, parameters `'P'`, sync `'S'`,
  dropouts `'O'`
- Zero blocking for producers: a failed write only copies nothing and returns
  `false`; overflow is recorded as a ULog dropout with its duration
- Message-atomic ring buffer: a torn write never corrupts the stream, the
  drained data always contains whole messages
- Nested formats, fixed arrays, trailing `_padding` handling, timestamp
  monotonicity enforced per subscription
- Optional LZ4 compression producing standard `.lz4` files
- No dynamic allocation in the encoder; fixed-capacity format registry

## Usage

Everything starts with a `Sink`. On the host a `RamSink` or `FileSink` is
enough; on ESP32 wrap a `LittleFsSink`, optionally in an `Lz4Sink`.

### Synchronous (single task, host or bare metal)

```cpp
ulog::FileSink sink("/tmp/log.ulg");
ulog::Writer logger{sink};                        // clock: esp_timer / steady_clock

ulog::Writer::Config config;                     // or inject your own clock
config.now_us = my_clock;
ulog::Writer logger2{sink, config};

struct __attribute__((packed)) Imu { uint64_t timestamp; float gyro_rad[3]; };

auto imu = logger.declare("sensor_imu",
                          "uint64_t timestamp;float[3] gyro_rad;");
if (imu) imu.log(Imu{esp_timer_get_time(), {0.1f, 0.f, 0.f}});

logger.log_text(ulog::Level::Info, "hello");
logger.add_info("ver_hw", "seeed_xiao_esp32s3");
logger.add_param("pid_kp", 1.5f);
```

The format string is written verbatim as the ULog `'F'` message, so field
names and types stay identical to what analysis tools expect. Payload structs
must be trivially copyable and match the computed size (use `__attribute__((packed))`
when the `uint64_t timestamp` is not the first member).

### Asynchronous (ESP32, FreeRTOS)

`AsyncWriter` owns a ring buffer and a drain task; producers only memcpy and
never block. When the ring overflows, whole messages are dropped and the lost
time is reported with `'O'` dropout messages.

```cpp
ulog::LittleFsSink flash{"/log001.ulg.lz4"};
flash.open();
ulog::Lz4Sink sink{flash};                       // optional

ulog::AsyncWriter::Config cfg;
cfg.ring_storage = heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM);
cfg.ring_size = 64 * 1024;
// cfg.task_core = 0;                             // pin drain task, default: none

ulog::AsyncWriter logger{sink, ulog::Writer::Config(), cfg};
logger.start();
auto imu = logger.declare("sensor_imu", "uint64_t timestamp;float[3] gyro_rad;");
imu.log(sample);                                  // ~µs, never blocks

logger.stop();      // flush + join (stop producers first)
sink.finish();      // LZ4 frame footer
flash.close();
```

A complete firmware example lives in `src/main.cpp` (PlatformIO,
`seeed_xiao_esp32s3`). It uses the `Logger` flight-recorder facade
(`src/logger.hpp`): `begin()` records into RAM while running — no flash
stalls, full 200 Hz on the example loop — and `write_to_flash()` persists
the whole session to LittleFS once producers are stopped.

### Getting logs off the device

`tools/pull_littlefs.py` reads the LittleFS partition over serial with esptool
and extracts every file to a host directory:

```
python3 tools/pull_littlefs.py --port /dev/ttyACM0 --out logs/
python3 tools/pull_littlefs.py --image littlefs.bin --out logs/   # existing dump
lz4 -d logs/log001.ulg.lz4 logs/log001.ulg                        # if compressed
```

It auto-detects the DATA/SPIFFS partition from the partition table
(`--label spiffs` to pick one explicitly, `--keep-dump` to keep the raw
image). Requires `esptool` and `pip install littlefs-python`.

### Compression

`Lz4Sink` wraps any other sink and produces a standard LZ4 frame — the file
on flash is `log001.ulg.lz4`, not a `.ulg` (the ULog spec has no in-format
compression, so tools will never read a compressed stream directly):

```
host$ lz4 -d log001.ulg.lz4 log001.ulg   # one command, instant
host$ pyulog info log001.ulg            # standard toolchain
```

Trade-offs: typical ratio 2-4x on sensor data, negligible CPU on the drain
task at embedded rates, but a crash loses the tail since the last flush (the
drain task flushes when a burst completes; raise `flush_interval_ms` to batch
more and touch the flash less) and a corrupted 64 KB block ends recovery
there. Blocks are independent by default so a corrupted block does not
compromise the following ones. RAM cost is about 130 KB (LZ4 context +
compression buffer), internal heap.

## Layout

```
lib/ulog/               the library
  include/ulog/
    writer.hpp          Writer, Message, WriterTarget/WriterLock, Level
    sink.hpp            Sink interface
    ram_sink.hpp        in-memory sink (tests)
    file_sink.hpp       host stdio sink
    littlefs_sink.hpp   ESP32 LittleFS sink, optional size cap
    lz4_sink.hpp        LZ4 frame compression wrapper
    ring.hpp            SPSC ring buffer + RingWriterTarget (drop accounting)
    async.hpp           AsyncWriter + FreeRtosLock (ESP32)
    format.hpp          ULog field-list parser
  src/                 encoder + parser implementation
lib/lz4/                vendored lz4 1.9.4 (BSD-2, see lib/lz4/LICENSE)
test/                   Unity tests (native) + pyulog validation script
src/main.cpp            ESP32-S3 example firmware
```

## Testing

Everything except the two ESP32-only headers runs on the host:

```
pio test -e native                       # 33 Unity tests
python3 test/pyulog_check.py              # sync round-trip, strict expectations
python3 test/pyulog_check.py --profile async   # ring drops -> 'O' messages
python3 test/pyulog_check.py --profile lz4     # LZ4 frames: round-trip, multi-block,
                                               # checksum/HC configs, crash-prefix recovery
```

The Unity tests themselves write the `.ulg` files that the pyulog script then
validates, so the byte-level checks and the third-party-parser check cover the
same artifacts.

Requires `pyulog` and `lz4` (python bindings) in the Python environment.

## Design notes and limits

- `Writer` is not thread-safe by itself; it takes an optional `WriterLock`
  (`AsyncWriter` installs a FreeRTOS mutex). Every public call is serialized.
- Format registry: max 32 formats, message names up to 23 chars, payload
  capped at 64 KB by the ULog message size field.
- If the sink fails persistently (e.g. disk full), the drain task discards
  data without a dropout marker; log rotation is not implemented yet
  (`LittleFsSink` only enforces an optional size cap).
- Crash data appending (`incompat_flags` + `appended_offsets`) is not
  implemented.
