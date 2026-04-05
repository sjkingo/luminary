#pragma once

#include "drivers/blkdev.h"

/* Read the MBR partition table from disk and register any non-empty
 * partitions as child blkdevs (e.g. /dev/hda1 through /dev/hda4).
 * Called by init_ata() after registering each whole-disk blkdev. */
void part_probe(struct blkdev *disk);
