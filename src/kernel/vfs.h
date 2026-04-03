#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ── node type flags ─────────────────────────────────────────────────────── */
#define VFS_FILE    0x01
#define VFS_DIR     0x02

/* ── maximum path/name lengths ───────────────────────────────────────────── */
#define VFS_NAME_MAX  128
#define VFS_PATH_MAX  256
#define VFS_FD_MAX     32   /* open file descriptors per task */

/* ── directory entry (returned by readdir) ───────────────────────────────── */
struct vfs_dirent {
    char     name[VFS_NAME_MAX];
    uint32_t inode;
    uint8_t  type;      /* VFS_FILE or VFS_DIR */
};

/* ── vfs node ─────────────────────────────────────────────────────────────── */
struct vfs_node {
    char     name[VFS_NAME_MAX];
    uint32_t inode;
    uint32_t size;      /* bytes (files only) */
    uint8_t  flags;     /* VFS_FILE | VFS_DIR */

    /* File data pointer (for initrd-backed files: pointer into cpio data) */
    const uint8_t *data;

    /* Children (directories only): singly-linked list */
    struct vfs_node *children;  /* first child */
    struct vfs_node *sibling;   /* next sibling */
    struct vfs_node *parent;
};

/* ── open file descriptor ─────────────────────────────────────────────────── */
struct vfs_fd {
    bool          open;
    struct vfs_node *node;
    uint32_t      offset;   /* current read position (files) */
    uint32_t      dir_idx;  /* current child index (dirs, for readdir) */
};

/* ── kernel-facing API ───────────────────────────────────────────────────── */

/* Set the root of the VFS tree */
void vfs_set_root(struct vfs_node *root);

/* Resolve an absolute path to a node. Returns NULL if not found. */
struct vfs_node *vfs_lookup(const char *path);

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

/* Allocate a new node from the node pool (used by initrd). Returns NULL if
 * the pool is exhausted. */
struct vfs_node *vfs_alloc_node(void);
