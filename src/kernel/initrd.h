#pragma once

#include <stdint.h>

/* Register the initrd filesystem driver and save the cpio data pointer.
 * Must be called before vfs_do_mount("...", "initrd"). */
void init_initrd(const void *data, uint32_t size);

/* Look up a file by absolute path in the VFS tree. Returns a pointer into
 * the cpio data (zero-copy) and fills *size_out. Returns NULL if not found.
 * Only valid after the initrd has been mounted as the VFS root. */
const void *initrd_get_file(const char *path, uint32_t *size_out);
