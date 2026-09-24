#!/usr/bin/env python3
"""Validates .ulg files produced by the native test suite with pyulog.

Usage:
    python3 test/pyulog_check.py [--profile sync|async] [path]

Defaults: sync -> /tmp/ulog_roundtrip.ulg, async -> /tmp/ulog_async_roundtrip.ulg
"""

import argparse
import sys

import numpy as np
from pyulog import ULog


def check_sync(path: str) -> None:
    ulog = ULog(path)
    assert ulog.file_corruption is False, "pyulog reported file corruption"
    assert not ulog.dropouts, f"unexpected dropouts: {ulog.dropouts}"

    assert ulog.start_timestamp == 1000, ulog.start_timestamp
    assert ulog.last_timestamp == 10990, ulog.last_timestamp

    imu = ulog.get_dataset("sensor_imu")
    baro = ulog.get_dataset("sensor_baro")
    assert len(imu.data["timestamp"]) == 100
    assert len(baro.data["timestamp"]) == 50
    assert imu.data["timestamp"][0] == 10000
    assert imu.data["timestamp"][-1] == 10990
    assert abs(imu.data["accel_mps2[0]"][0] - 9.81) < 1e-5
    assert abs(baro.data["pressure_pa"][10] - 101315.0) < 0.5

    assert ulog.msg_info_dict.get("sys_name") == "ulog-lib", ulog.msg_info_dict
    assert ulog.msg_info_dict.get("ver_sw_release") == 0x000100FF

    assert ulog.initial_parameters == {"pid_kp": 1.5, "mode": 3}

    texts = [(m.log_level, m.message) for m in ulog.logged_messages]
    assert texts == [(6, "host round-trip log"), (4, "synthetic data")], texts

    print(f"OK: {path} is a valid ULog file (sync)")
    print(f"    start={ulog.start_timestamp} last={ulog.last_timestamp} "
          f"imu={len(imu.data['timestamp'])} baro={len(baro.data['timestamp'])}")


def check_async(path: str) -> None:
    ulog = ULog(path)
    assert ulog.file_corruption is False, "pyulog reported file corruption"

    imu = ulog.get_dataset("sensor_imu")
    ts = imu.data["timestamp"]
    assert 0 < len(ts) <= 200, len(ts)
    assert np.all(np.diff(ts) > 0), "timestamps not strictly increasing"

    assert ulog.msg_info_dict.get("sys_name") == "ulog-async"

    assert len(ulog.dropouts) > 0, "expected dropout messages"
    total_dropped = sum(d.duration for d in ulog.dropouts)
    assert total_dropped > 0, total_dropped

    print(f"OK: {path} is a valid ULog file (async)")
    print(f"    imu={len(ts)} samples, {len(ulog.dropouts)} dropout(s), "
          f"{total_dropped} ms dropped")


def check_lz4(path: str) -> None:
    import lz4.frame

    with open(path, "rb") as f:
        compressed = f.read()
    decompressed = lz4.frame.decompress(compressed)
    assert len(decompressed) > 0

    out = path[:-len(".lz4")] if path.endswith(".lz4") else "/tmp/ulog_lz4_roundtrip.ulg"
    with open(out, "wb") as f:
        f.write(decompressed)
    print(f"OK: {path} is a valid LZ4 frame "
          f"({len(compressed)} -> {len(decompressed)} bytes, "
          f"{100 - 100 * len(compressed) // len(decompressed)}% saved)")
    check_sync(out)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", choices=["sync", "async", "lz4"], default="sync")
    parser.add_argument("path", nargs="?")
    args = parser.parse_args()

    path = args.path
    if path is None:
        path = {  # noqa: C408 - explicit dict is fine here
            "sync": "/tmp/ulog_roundtrip.ulg",
            "async": "/tmp/ulog_async_roundtrip.ulg",
            "lz4": "/tmp/ulog_lz4_roundtrip.ulg.lz4",
        }[args.profile]

    if args.profile == "sync":
        check_sync(path)
    elif args.profile == "async":
        check_async(path)
    else:
        check_lz4(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
