#!/usr/bin/env python3
"""Normalize FAT16 directory timestamps without changing file contents/layout."""
import datetime
import os
import struct
import sys
from pathlib import Path


def normalize(path, epoch):
    data = bytearray(Path(path).read_bytes())
    sector, cluster = struct.unpack_from('<H', data, 11)[0], data[13]
    reserved, fats, roots = struct.unpack_from('<HBH', data, 14)
    fat_size = struct.unpack_from('<H', data, 22)[0]
    if data[54:62] != b'FAT16   ' or sector not in (512, 4096) or not cluster or not fat_size:
        raise ValueError('expected a FAT16 image')
    root = (reserved + fats * fat_size) * sector
    heap = root + ((roots * 32 + sector - 1) // sector) * sector
    fat = reserved * sector
    stamp = datetime.datetime.fromtimestamp(max(315532800, epoch), datetime.timezone.utc)
    date = ((min(stamp.year, 2107) - 1980) << 9) | (stamp.month << 5) | stamp.day
    time = (stamp.hour << 11) | (stamp.minute << 5) | (stamp.second // 2)
    seen = set()

    def directory(ranges):
        for offset, length in ranges:
            for at in range(offset, offset + length, 32):
                if data[at] == 0:
                    return
                if data[at] == 0xE5 or data[at + 11] == 0x0F or data[at + 11] & 8:
                    continue
                data[at + 13] = 0
                struct.pack_into('<HHH', data, at + 14, time, date, date)
                struct.pack_into('<HH', data, at + 22, time, date)
                if data[at + 11] & 16 and data[at] != ord('.'):
                    first = struct.unpack_from('<H', data, at + 26)[0]
                    chain = []
                    while 2 <= first < 0xFFF8:
                        if first in seen:
                            raise ValueError('cyclic/cross-linked FAT directory')
                        seen.add(first)
                        start = heap + (first - 2) * cluster * sector
                        if start + cluster * sector > len(data):
                            raise ValueError('directory outside image')
                        chain.append((start, cluster * sector))
                        first = struct.unpack_from('<H', data, fat + first * 2)[0]
                    directory(chain)

    directory([(root, roots * 32)])
    Path(path).write_bytes(data)


if __name__ == '__main__':
    normalize(sys.argv[1], int(os.environ.get('SOURCE_DATE_EPOCH', '1704067200')))
