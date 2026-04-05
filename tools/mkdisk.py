#!/usr/bin/env python3
"""Write an MBR partition table into an existing raw disk image.

Creates one primary partition (type 0x83, Linux) starting at LBA 2048,
spanning the rest of the image. Assumes the image already exists and is
large enough (created by dd beforehand).

Usage: mkdisk.py <image>
"""
import sys
import struct


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <image>")
        sys.exit(1)

    img = sys.argv[1]
    total_sectors = 204800   # must match the dd count in the Makefile
    lba_start     = 2048
    lba_count     = total_sectors - lba_start

    # MBR partition entry: status(1) + CHS_first(3) + type(1) + CHS_last(3)
    #                      + lba_start(4) + lba_count(4)
    # CHS fields are ignored in LBA mode; zero them.
    entry = struct.pack('<B3sB3sII',
                        0x80,              # status: bootable
                        b'\x00\x00\x00',   # CHS first (ignored)
                        0x83,              # type: Linux
                        b'\x00\x00\x00',   # CHS last (ignored)
                        lba_start,
                        lba_count)

    with open(img, 'r+b') as f:
        f.seek(446)
        f.write(entry)
        f.seek(510)
        f.write(b'\x55\xaa')

    print(f"mkdisk: wrote MBR to {img} "
          f"(partition 1: LBA {lba_start}+{lba_count}, type 0x83)")


if __name__ == '__main__':
    main()
