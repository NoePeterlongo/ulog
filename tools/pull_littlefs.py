#!/usr/bin/env python3
"""Dump the LittleFS partition of an ESP32 with esptool and extract all files.

Two modes:
  --port   : read the partition table via esptool, find the LittleFS
             (DATA/SPIFFS) partition, dump it, then extract every file.
  --image  : skip esptool and extract from an existing partition dump
             (e.g. produced by `esptool read_flash` by hand).

Usage:
    python3 tools/pull_littlefs.py --port /dev/ttyACM0 --out logs/
    python3 tools/pull_littlefs.py --image littlefs.bin --out logs/
    python3 tools/pull_littlefs.py --port /dev/ttyACM0 --label spiffs

Requires: esptool (CLI or `pip install esptool`) and `pip install
littlefs-python`.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_SIZE = 0x1000
ENTRY_SIZE = 32
DATA_TYPE = 0x01
SPIFFS_SUBTYPE = 0x82   # classic SPIFFS subtype; Arduino LittleFS mounts this
LITTLEFS_SUBTYPE = 0x83  # explicit LITTLEFS subtype (esp-idf partition CSVs)

BLOCK_SIZE = 4096  # ESP32 littlefs block size = flash erase sector


def find_esptool():
    candidates = []
    if os.environ.get("ESPTOOL"):
        candidates.append(os.environ["ESPTOOL"].split())
    for name in ("esptool", "esptool.py"):
        path = shutil.which(name)
        if path:
            candidates.append([path])
    pio_tool = os.path.expanduser(
        "~/.platformio/packages/tool-esptoolpy/esptool.py")
    if os.path.exists(pio_tool):
        candidates.append([sys.executable, pio_tool])
    try:
        import esptool  # noqa: F401
        candidates.append([sys.executable, "-m", "esptool"])
    except ImportError:
        pass
    for candidate in candidates:
        try:
            result = subprocess.run(candidate + ["version"],
                                     capture_output=True, check=True)
            print(f"using esptool: {' '.join(candidate)} "
                  f"({result.stdout.decode().strip().splitlines()[0]})")
            return candidate
        except (OSError, subprocess.CalledProcessError):
            continue
    sys.exit("esptool not found: install it (pip install esptool), "
             "set ESPTOOL=/path/to/esptool, or use --image")


def run_esptool(esptool, args, cmd_args):
    cmd = esptool + ["--chip", args.chip]
    if args.port:
        cmd += ["--port", args.port]
    if args.baud:
        cmd += ["--baud", str(args.baud)]
    subprocess.run(cmd + cmd_args, check=True)


def parse_partition_table(data):
    partitions = []
    for offset in range(0, len(data) - ENTRY_SIZE + 1, ENTRY_SIZE):
        entry = data[offset:offset + ENTRY_SIZE]
        if entry[0:2] != b"\xaa\x50":
            if entry[0:2] == b"\xeb\xeb":  # end marker
                break
            continue
        ptype, subtype = entry[2], entry[3]
        p_offset, p_size = struct.unpack("<II", entry[4:12])
        label = entry[12:28].rstrip(b"\x00").decode("ascii", "replace")
        partitions.append({
            "type": ptype, "subtype": subtype,
            "offset": p_offset, "size": p_size, "label": label,
        })
    return partitions


def find_littlefs_partition(partitions, label):
    wanted = (SPIFFS_SUBTYPE, LITTLEFS_SUBTYPE)
    data = [p for p in partitions
            if p["type"] == DATA_TYPE and p["subtype"] in wanted]
    if label:
        matches = [p for p in data if p["label"] == label]
        if not matches:
            sys.exit(f"no LittleFS (DATA/SPIFFS/LITTLEFS) partition labeled "
                     f"'{label}'")
        return matches[0]
    if not data:
        sys.exit("no LittleFS (DATA/SPIFFS) partition found; "
                 "use --label or --offset/--size")
    if len(data) > 1:
        print("several DATA/SPIFFS partitions found, using the first; "
              "use --label to pick another:", [p["label"] for p in data])
    return data[0]


def extract(image_path, partition_size, out_dir, block_size):
    from littlefs import LittleFS
    from littlefs.context import UserContextFile

    block_count = partition_size // block_size
    if block_count * block_size != partition_size:
        sys.exit(f"partition size {partition_size} is not a multiple of "
                 f"the block size {block_size}")

    context = UserContextFile(image_path)
    fs = LittleFS(context=context, block_size=block_size,
                  block_count=block_count)

    extracted = []
    for root, dirs, files in fs.walk("/"):
        for name in files:
            lfs_path = ("/" + name) if root == "/" else f"{root}/{name}"
            rel_path = lfs_path.lstrip("/")
            if any(part == ".." for part in rel_path.split("/")):
                sys.exit(f"refusing unsafe path in image: {lfs_path}")
            dest = os.path.join(out_dir, rel_path)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with fs.open(lfs_path, "rb") as src, open(dest, "wb") as dst:
                dst.write(src.read())
            extracted.append((rel_path, os.path.getsize(dest)))

    return extracted


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="serial port, e.g. /dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument("--chip", default="esp32s3")
    parser.add_argument("--label", help="LittleFS partition label (default: first DATA/SPIFFS)")
    parser.add_argument("--image", help="extract from an existing partition dump instead of the device")
    parser.add_argument("--offset", type=lambda x: int(x, 0), help="partition offset override (flash mode)")
    parser.add_argument("--size", type=lambda x: int(x, 0), help="partition size override (flash mode)")
    parser.add_argument("--block-size", type=int, default=BLOCK_SIZE)
    parser.add_argument("--out", default="littlefs_files", help="output directory")
    parser.add_argument("--keep-dump", metavar="PATH", help="keep the raw partition dump at PATH")
    args = parser.parse_args()

    if not args.image and not args.port:
        parser.error("either --port or --image is required")

    os.makedirs(args.out, exist_ok=True)
    temp_dir = None
    try:
        if args.image:
            image_path = args.image
            partition_size = (args.size if args.size
                              else os.path.getsize(image_path))
        else:
            esptool = find_esptool()
            temp_dir = tempfile.mkdtemp(prefix="pull_littlefs_")
            image_path = os.path.join(temp_dir, "littlefs.bin")

            if args.offset is not None and args.size is not None:
                partition = {"offset": args.offset, "size": args.size,
                             "label": args.label or "(manual)"}
            else:
                table_path = os.path.join(temp_dir, "partitions.bin")
                print(f"reading partition table at 0x{PARTITION_TABLE_OFFSET:X}...")
                run_esptool(esptool, args, [
                    "read_flash", hex(PARTITION_TABLE_OFFSET),
                    hex(PARTITION_TABLE_SIZE), table_path])
                with open(table_path, "rb") as f:
                    partitions = parse_partition_table(f.read())
                partition = find_littlefs_partition(partitions, args.label)

            print(f"LittleFS partition '{partition['label']}': "
                  f"offset 0x{partition['offset']:X}, "
                  f"size {partition['size']} bytes")
            print("dumping partition (this resets the chip)...")
            run_esptool(esptool, args, [
                "read_flash", hex(partition["offset"]),
                hex(partition["size"]), image_path])
            partition_size = partition["size"]

        extracted = extract(image_path, partition_size, args.out,
                            args.block_size)

        if temp_dir and args.keep_dump:
            shutil.copy(image_path, args.keep_dump)
            print(f"raw partition dump kept at {args.keep_dump}")
    finally:
        if temp_dir:
            shutil.rmtree(temp_dir, ignore_errors=True)

    if not extracted:
        print(f"no files found in the image "
              f"(mounted as block_size={args.block_size}, "
              f"block_count={partition_size // args.block_size})")
        return 1

    total = 0
    for path, size in sorted(extracted):
        total += size
        print(f"  {size:>10}  {path}")
    print(f"{len(extracted)} file(s), {total} bytes -> {args.out}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
