#pragma once

#include <stdint.h>

struct vfs_node;

/* Parse a cpio newc archive at 'data' of 'size' bytes. Builds the VFS tree
 * and returns the root node. Does NOT call vfs_set_root — the caller decides
 * whether to use this as the VFS root or ignore it.
 * Fills *file_count_out with the number of regular files found. */
struct vfs_node *initrd_init(const void *data, uint32_t size,
                             uint32_t *file_count_out);

/* Look up a file by absolute path in the initrd tree. Returns a pointer into
 * the cpio data (zero-copy) and fills *size_out. Returns NULL if not found.
 * Only valid after initrd_init has been called and the initrd root is
 * set as the VFS root. */
const void *initrd_get_file(const char *path, uint32_t *size_out);
