#!/usr/bin/env python3
"""Append Memento window metadata from stdin to an r2 ELF (no loaded bytes change)."""

import json
from pathlib import Path
import struct
import sys

SIGNATURE = b"R2MEMENTOAPP\0\0\0\0"
SIZE = 128


def trailer(manifest):
    magic, version = manifest["magic"], manifest["version"]
    width, height, flags = manifest["width"], manifest["height"], manifest["flags"]
    if any(type(value) is not int for value in (magic, version, width, height, flags)):
        raise ValueError("manifest numbers must be integers")
    if not isinstance(manifest["title"], str):
        raise ValueError("manifest title must be a string")
    title = manifest["title"].encode("ascii")
    if not 0 < magic <= 0xffffffff or version != 1:
        raise ValueError("unsupported hosted-window magic or version")
    if not 0 < width <= 2048 or not 0 < height <= 2048 or flags & ~3:
        raise ValueError("invalid window dimensions or flags")
    if not 0 < len(title) < 80 or any(ch < 32 or ch > 126 for ch in title):
        raise ValueError("title must contain 1..79 printable ASCII bytes")
    return struct.pack("<16s7I80sI", SIGNATURE, 1, SIZE, magic, version, width, height, flags, title, 0)


def attach(image, metadata):
    if len(image) < 64 or image[:7] != b"\x7fELF\x02\x01\x01":
        raise ValueError("expected a little-endian ELF64 executable")
    kind, machine, version = struct.unpack_from("<HHI", image, 16)
    header_size, entry_size, entries = struct.unpack_from("<HHH", image, 52)
    offset = struct.unpack_from("<Q", image, 32)[0]
    if (kind, machine, version, header_size) != (2, 62, 1, 64) or entry_size != 56 or not entries:
        raise ValueError("expected an r2 x86-64 executable")
    if offset + entries * entry_size > len(image):
        raise ValueError("truncated ELF program headers")
    end = len(image)
    if end >= SIZE and image[-SIZE:-SIZE+len(SIGNATURE)] == SIGNATURE:
        end -= SIZE  # replace a previous trailer rather than stacking them
    for i in range(entries):
        entry = offset + i * entry_size
        if struct.unpack_from("<I", image, entry)[0] != 1:
            continue
        file_offset = struct.unpack_from("<Q", image, entry+8)[0]
        file_size = struct.unpack_from("<Q", image, entry+32)[0]
        if file_offset + file_size > end:
            raise ValueError("truncated ELF load segment")
    return image[:end] + trailer(metadata)


def main():
    if len(sys.argv) != 2:
        raise ValueError("usage: append_manifest.py app.elf < manifest.json")
    path = Path(sys.argv[1])
    image = attach(path.read_bytes(), json.load(sys.stdin))
    path.write_bytes(image)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, TypeError, OSError, struct.error) as exc:
        sys.exit("Memento manifest: " + str(exc))
