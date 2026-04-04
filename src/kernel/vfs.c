/* Virtual filesystem layer.
 *
 * Provides a simple in-memory VFS tree. The initrd driver populates the tree
 * from a cpio archive; the tree is then mounted at "/". Syscall handlers call
 * into this layer; vfs_fd structs are stored per-task in the task struct.
 *
 * Writable files (created via vfs_creat) own a heap-allocated buffer that
 * grows on demand. Writes past the current end of file extend size; the buffer
 * is reallocated in power-of-two steps to amortise copies.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "kernel/vfs.h"
#include "kernel/heap.h"
#include "kernel/kernel.h"

#define MODULE "vfs: "

/* ── node pool ───────────────────────────────────────────────────────────── */
/* Static pool — no heap allocation needed for the VFS tree itself. */
#define VFS_NODE_POOL_SIZE 512

static struct vfs_node node_pool[VFS_NODE_POOL_SIZE];
static uint32_t        node_pool_used = 0;

struct vfs_node *vfs_alloc_node(void)
{
    if (node_pool_used >= VFS_NODE_POOL_SIZE) {
        printk(MODULE "node pool exhausted\n");
        return NULL;
    }
    struct vfs_node *n = &node_pool[node_pool_used++];
    memset(n, 0, sizeof(*n));
    return n;
}

/* ── root ────────────────────────────────────────────────────────────────── */
static struct vfs_node *vfs_root = NULL;

void vfs_set_root(struct vfs_node *root)
{
    vfs_root = root;
    printk(MODULE "root mounted\n");
}

/* ── path resolution ─────────────────────────────────────────────────────── */

/* Walk one path component. Looks for a child of dir named 'name' (not
 * null-terminated — length is given by len). */
static struct vfs_node *dir_child(struct vfs_node *dir,
                                  const char *name, uint32_t len)
{
    struct vfs_node *c = dir->children;
    while (c) {
        uint32_t clen = (uint32_t)strlen(c->name);
        if (clen == len && memcmp(c->name, name, len) == 0)
            return c;
        c = c->sibling;
    }
    return NULL;
}

struct vfs_node *vfs_lookup(const char *path)
{
    if (!vfs_root) return NULL;
    if (!path || path[0] != '/') return NULL;

    struct vfs_node *cur = vfs_root;
    const char *p = path + 1; /* skip leading '/' */

    while (*p) {
        /* skip consecutive slashes */
        while (*p == '/') p++;
        if (!*p) break;

        /* find end of this component */
        const char *end = p;
        while (*end && *end != '/') end++;

        uint32_t len = (uint32_t)(end - p);
        if (len == 0) break;

        if (!(cur->flags & VFS_DIR)) return NULL; /* not a directory */

        cur = dir_child(cur, p, len);
        if (!cur) return NULL;

        p = end;
    }
    return cur;
}

/* ── read ────────────────────────────────────────────────────────────────── */
uint32_t vfs_read(struct vfs_node *node, uint32_t offset,
                  uint32_t len, void *buf)
{
    if (!node) return 0;
    if (node->flags & VFS_CHARDEV) {
        if (!node->read_op) return 0;
        return node->read_op(offset, len, buf);
    }
    if (!(node->flags & VFS_FILE) || !node->data) return 0;
    if (offset >= node->size) return 0;
    uint32_t avail = node->size - offset;
    if (len > avail) len = avail;
    memcpy(buf, node->data + offset, len);
    return len;
}

/* ── write ───────────────────────────────────────────────────────────────── */
uint32_t vfs_write(struct vfs_node *node, uint32_t offset,
                   uint32_t len, const void *buf)
{
    if (!node) return 0;
    if (node->flags & VFS_CHARDEV) {
        if (!node->write_op) return 0;
        return node->write_op(offset, len, buf);
    }
    if (!(node->flags & VFS_FILE) || !node->writable) return 0;

    uint32_t end = offset + len;
    if (end < offset) return 0; /* overflow */

    /* Grow buffer if needed */
    if (end > node->buf_cap) {
        uint32_t newcap = node->buf_cap ? node->buf_cap : 64;
        while (newcap < end) newcap *= 2;
        uint8_t *newbuf = (uint8_t *)kmalloc(newcap);
        if (!newbuf) return 0;
        if (node->size > 0 && node->data)
            memcpy(newbuf, node->data, node->size);
        if (node->buf_cap > 0)
            kfree((void *)node->data);
        node->data    = newbuf;
        node->buf_cap = newcap;
    }

    memcpy((uint8_t *)node->data + offset, buf, len);
    if (end > node->size)
        node->size = end;
    return len;
}

/* ── vfs_creat ───────────────────────────────────────────────────────────── */
struct vfs_node *vfs_creat(const char *path)
{
    if (!vfs_root || !path || path[0] != '/') return NULL;

    /* Split into parent path and basename */
    const char *base = path;
    for (const char *s = path; *s; s++)
        if (*s == '/') base = s + 1;
    if (*base == '\0') return NULL; /* can't create root */

    uint32_t parent_len = (uint32_t)(base - path);
    /* parent_len includes the trailing slash; strip it */
    if (parent_len > 1) parent_len--;

    /* Look up parent directory */
    struct vfs_node *parent;
    if (parent_len == 0) {
        parent = vfs_root; /* file at root */
    } else {
        char parent_path[VFS_PATH_MAX];
        if (parent_len >= VFS_PATH_MAX) return NULL;
        memcpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';
        parent = vfs_lookup(parent_path);
    }
    if (!parent || !(parent->flags & VFS_DIR)) return NULL;

    /* Find or allocate node */
    struct vfs_node *n = dir_child(parent, base, (uint32_t)strlen(base));
    if (n) {
        /* Truncate existing file */
        if (!(n->flags & VFS_FILE)) return NULL;
        if (n->writable && n->buf_cap > 0) {
            kfree((void *)n->data);
            n->data    = NULL;
            n->buf_cap = 0;
        } else {
            n->data    = NULL;
            n->buf_cap = 0;
        }
        n->size     = 0;
        n->writable = true;
        return n;
    }

    n = vfs_alloc_node();
    if (!n) return NULL;
    uint32_t blen = (uint32_t)strlen(base);
    if (blen >= VFS_NAME_MAX) blen = VFS_NAME_MAX - 1;
    memcpy(n->name, base, blen);
    n->name[blen] = '\0';
    n->flags    = VFS_FILE;
    n->writable = true;
    vfs_add_child(parent, n);
    return n;
}

/* ── tree helpers ─────────────────────────────────────────────────────────── */
void vfs_add_child(struct vfs_node *parent, struct vfs_node *child)
{
    child->parent  = parent;
    child->sibling = NULL;
    if (!parent->children) {
        parent->children = child;
        return;
    }
    struct vfs_node *s = parent->children;
    while (s->sibling) s = s->sibling;
    s->sibling = child;
}

struct vfs_node *vfs_get_root(void)
{
    return vfs_root;
}

/* ── readdir ─────────────────────────────────────────────────────────────── */
struct vfs_node *vfs_readdir(struct vfs_node *node, uint32_t index)
{
    if (!node || !(node->flags & VFS_DIR)) return NULL;
    struct vfs_node *c = node->children;
    uint32_t i = 0;
    while (c) {
        if (i == index) return c;
        i++;
        c = c->sibling;
    }
    return NULL;
}

/* ── stat ────────────────────────────────────────────────────────────────── */
int vfs_stat(const char *path, struct vfs_stat *out)
{
    struct vfs_node *n = vfs_lookup(path);
    if (!n) return -1;
    out->size = n->size;
    out->type = n->flags;
    return 0;
}
