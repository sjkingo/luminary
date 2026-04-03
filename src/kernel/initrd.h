#pragma once

#include <stdint.h>

/* Parse a cpio newc archive at 'data' of 'size' bytes and mount it
 * as the VFS root. Called once from kernel_main after detecting the
 * initrd multiboot module. */
void initrd_init(const void *data, uint32_t size);

/* Look up a file by absolute path. Returns a pointer into the cpio data
 * (zero-copy) and fills *size_out. Returns NULL if not found. */
const void *initrd_get_file(const char *path, uint32_t *size_out);
