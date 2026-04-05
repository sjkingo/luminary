#!/bin/sh
# ext2dump.sh — dump a file from the ext2 partition in _build/disk.img
# Usage: tools/ext2dump.sh /path/to/file
# Example: tools/ext2dump.sh /hello

set -e

if [ -z "$1" ]; then
    echo "usage: $0 /path/to/file" >&2
    exit 1
fi

DISK_IMG="_build/disk.img"
PART_IMG="/tmp/luminary_part.img"
DEBUGFS="/opt/homebrew/opt/e2fsprogs/sbin/debugfs"
PART_LBA=2048

if [ ! -f "$DISK_IMG" ]; then
    echo "ext2dump: $DISK_IMG not found" >&2
    exit 1
fi

dd if="$DISK_IMG" of="$PART_IMG" bs=512 skip=$PART_LBA 2>/dev/null
"$DEBUGFS" -R "cat $1" "$PART_IMG" 2>/dev/null | hexdump -C
