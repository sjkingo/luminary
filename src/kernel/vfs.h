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
 *
 * Mount points:
 *   When a filesystem is mounted at a directory node, that node's mounted_root
 *   is set to the root node of the mounted filesystem. vfs_lookup follows
 *   mounted_root at each step, so paths transparently traverse mount points.
 *   The node's fs pointer identifies the filesystem driver that owns it.
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

struct vfs_node;

/* Filesystem driver vtable. Registered via vfs_fs_register().
 * NULL ops fall back to the built-in VFS tree behaviour. */
struct fs_ops {
    /* Called when this fs is mounted at mountpoint. Should initialise the
     * subtree rooted at mountpoint->mounted_root. device is driver-specific
     * context (e.g. struct blkdev *); NULL for memory-only filesystems.
     * Returns 0 on success. */
    int (*mount)(struct vfs_node *mountpoint, void *device);

    /* Called when the fs is unmounted. Should free all nodes in the subtree
     * below mountpoint->mounted_root, then clear mounted_root. Returns 0 on
     * success, -1 if the mount point is busy (open fds exist underneath). */
    int (*umount)(struct vfs_node *mountpoint);
};

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
    bool           readonly;    /* true  → directory rejects creat/mkdir/unlink/write */
    bool           heap_alloc;  /* true  → node was kmalloc'd, not from static pool */
    uint32_t       buf_cap;     /* allocated capacity of data buffer */

    /* Character device I/O ops — NULL for regular files */
    uint32_t (*read_op)(uint32_t offset, uint32_t len, void *buf);
    uint32_t (*write_op)(uint32_t offset, uint32_t len, const void *buf);

    /* ioctl control op — NULL if device does not support control requests */
    int32_t (*control_op)(struct vfs_node *node, uint32_t request, void *arg);

    /* Mount point: non-NULL when another filesystem is mounted here.
     * vfs_lookup follows this pointer instead of walking children. */
    struct vfs_node *mounted_root;
    struct fs_ops   *fs;            /* filesystem driver that owns this node */

    /* Children (directories only): singly-linked list */
    struct vfs_node *children;  /* first child */
    struct vfs_node *sibling;   /* next sibling */
    struct vfs_node *parent;
};

/* ── open file descriptor ─────────────────────────────────────────────────── */
struct vfs_fd {
    bool          open;
    bool          append;    /* O_APPEND: writes always go to end of file */
    bool          nonblock;  /* O_NONBLOCK: reads return 0 instead of blocking */
    struct vfs_node *node;
    uint32_t      offset;   /* current read/write position (files) */
    uint32_t      dir_idx;  /* current child index (dirs, for readdir) */
};

/* ── kernel-facing API ───────────────────────────────────────────────────── */

/* Set the root of the VFS tree */
void vfs_set_root(struct vfs_node *root);

#define VFS_MOUNT_MAX 16
#define VFS_FS_MAX    8

struct vfs_mount_info {
    char path[VFS_PATH_MAX];
    char fstype[16];
    bool readonly;
};

/* Register a filesystem driver by name. Must be called before vfs_do_mount. */
void vfs_fs_register(const char *fstype, struct fs_ops *ops);

/* Mount filesystem fstype at path. path must name an existing directory.
 * Calls fs_ops->mount, records the mount, and updates the mount table.
 * Returns 0 on success, -1 on error. */
int vfs_do_mount(const char *path, const char *fstype, void *device);

/* Unmount the filesystem mounted at path. Calls fs_ops->umount.
 * Returns 0 on success, -1 if nothing is mounted there or unmount refused. */
int vfs_do_umount(const char *path);

/* Record a static (kernel-internal) mount in the mount table for display only.
 * Used by initrd/devfs which manage their own nodes directly. */
void vfs_mount(const char *path, const char *fstype, struct vfs_node *root);

/* Fill out[0..max-1] with registered mount info. Returns count filled. */
int vfs_get_mounts(struct vfs_mount_info *out, int max);

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
    uint32_t inode;
};
int vfs_stat(const char *path, struct vfs_stat *out);
int vfs_fstat(struct vfs_node *node, struct vfs_stat *out);

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

/* Rename or move old_path to new_path. Follows Linux rename(2) semantics:
 * - If new_path is an existing file it is replaced atomically.
 * - If new_path is an existing empty directory and old_path is a directory,
 *   new_path is replaced. Non-empty target directory returns -1.
 * - Moving a directory into itself (new_path is under old_path) returns -1.
 * Returns 0 on success, -1 on error. */
int vfs_rename(const char *old_path, const char *new_path);

/* Dispatch an ioctl control request to node->control_op.
 * Returns -1 if the node has no control_op. */
int32_t vfs_ioctl(struct vfs_node *node, uint32_t request, void *arg);

/* Allocate a chardev node, attach ops, and add it under /dev.
 * /dev must already exist (init_devfs must have run).
 * Returns the new node, or NULL on failure. */
struct vfs_node *vfs_register_dev(const char *name, uint32_t inode,
    uint32_t (*read_op)(uint32_t, uint32_t, void *),
    uint32_t (*write_op)(uint32_t, uint32_t, const void *),
    int32_t  (*control_op)(struct vfs_node *, uint32_t, void *));
