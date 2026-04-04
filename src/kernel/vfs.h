/* Virtual filesystem and file-descriptor layer.
 *
 * The VFS tree is an in-memory linked list of vfs_node structs allocated from
 * a static pool. initrd populates the tree at boot; writable files are created
 * at runtime with heap-backed buffers (non-persistent across reboots).
 *
 * Node types:
 *   VFS_FILE    — regular file; data points to a read-only cpio buffer, or to
 *                 a kmalloc'd buffer when writable is set.
 *   VFS_DIR     — directory; children/sibling chain.
 *   VFS_CHARDEV — character device; I/O dispatched through read_op/write_op.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define VFS_FILE    0x01
#define VFS_DIR     0x02
#define VFS_CHARDEV 0x04    /* character device — I/O via read_op/write_op */

#define VFS_NAME_MAX  128
#define VFS_PATH_MAX  256
#define VFS_FD_MAX     32   /* open file descriptors per task */

struct vfs_dirent {
    char     name[VFS_NAME_MAX];
    uint32_t inode;
    uint8_t  type;      /* VFS_FILE or VFS_DIR */
};

struct vfs_node {
    char     name[VFS_NAME_MAX];
    uint32_t inode;
    uint32_t size;      /* bytes of valid data (files only) */
    uint8_t  flags;     /* VFS_FILE | VFS_DIR | VFS_CHARDEV */

    /* File data.  For initrd-backed read-only files, data points directly
     * into the cpio image.  For writable files, data points to a kmalloc'd
     * buffer of capacity buf_cap; the node owns the allocation. */
    const uint8_t *data;
    bool           writable;    /* true  → heap-backed, mutable */
    uint32_t       buf_cap;     /* allocated capacity of data buffer */

    /* Character device I/O ops — NULL for regular files */
    uint32_t (*read_op)(uint32_t offset, uint32_t len, void *buf);
    uint32_t (*write_op)(uint32_t offset, uint32_t len, const void *buf);

    /* Children (directories only): singly-linked list */
    struct vfs_node *children;  /* first child */
    struct vfs_node *sibling;   /* next sibling */
    struct vfs_node *parent;
};

/* ── open file descriptor ─────────────────────────────────────────────────── */
struct vfs_fd {
    bool          open;
    bool          append;   /* O_APPEND: writes always go to end of file */
    struct vfs_node *node;
    uint32_t      offset;   /* current read/write position (files) */
    uint32_t      dir_idx;  /* current child index (dirs, for readdir) */
};

/* ── kernel-facing API ───────────────────────────────────────────────────── */

/* Set the root of the VFS tree */
void vfs_set_root(struct vfs_node *root);

/* Resolve an absolute path to a node. Returns NULL if not found. */
struct vfs_node *vfs_lookup(const char *path);

/* Resolve path relative to cwd into out_buf (VFS_PATH_MAX bytes).
 * Returns out_buf on success, NULL on overflow. */
const char *vfs_resolve(const char *cwd, const char *path, char *out_buf);

/* Read up to len bytes from node at offset. Returns bytes read. */
uint32_t vfs_read(struct vfs_node *node, uint32_t offset,
                  uint32_t len, void *buf);

/* Get the nth child of a directory node. Returns NULL if out of range. */
struct vfs_node *vfs_readdir(struct vfs_node *node, uint32_t index);

/* Stat a node by path. Fills *out. Returns 0 on success, -1 if not found. */
struct vfs_stat {
    uint32_t size;
    uint8_t  type;   /* VFS_FILE or VFS_DIR */
};
int vfs_stat(const char *path, struct vfs_stat *out);

/* Write up to len bytes to node at offset. Returns bytes written.
 * Only meaningful for VFS_CHARDEV nodes; regular files are read-only. */
uint32_t vfs_write(struct vfs_node *node, uint32_t offset,
                   uint32_t len, const void *buf);

/* Append child to parent's children list. Sets child->parent. */
void vfs_add_child(struct vfs_node *parent, struct vfs_node *child);

/* Return the current VFS root node. */
struct vfs_node *vfs_get_root(void);

/* Allocate a new node from the node pool. Checks the reclaim free-list first.
 * Returns NULL if the pool is exhausted. */
struct vfs_node *vfs_alloc_node(void);

/* Return a node to the reclaim free-list so it can be reused by vfs_alloc_node.
 * The node must not be referenced by any fd or tree pointer after this call. */
void vfs_free_node(struct vfs_node *n);

/* Create or truncate a writable file at path.  Intermediate directories must
 * already exist.  Returns the node on success or NULL on error.
 * The node's heap buffer grows on demand via vfs_write. */
struct vfs_node *vfs_creat(const char *path);

/* Create a new empty directory at path. Parent must exist. Returns the node
 * on success or NULL if the path already exists or the parent is missing. */
struct vfs_node *vfs_mkdir(const char *path);

/* Remove a regular file node at path. Frees its heap buffer if writable.
 * Returns 0 on success, -1 on error (not found, is a directory, is a chardev). */
int vfs_unlink(const char *path);
