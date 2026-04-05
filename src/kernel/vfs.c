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

static struct vfs_node  node_pool[VFS_NODE_POOL_SIZE];
static uint32_t         node_pool_used = 0;
static struct vfs_node *node_free_list = NULL; /* reclaimed nodes via sibling */

struct vfs_node *vfs_alloc_node(void)
{
    if (node_free_list) {
        struct vfs_node *n = node_free_list;
        node_free_list = n->sibling;
        memset(n, 0, sizeof(*n));
        return n;
    }
    if (node_pool_used < VFS_NODE_POOL_SIZE) {
        struct vfs_node *n = &node_pool[node_pool_used++];
        memset(n, 0, sizeof(*n));
        return n;
    }
    struct vfs_node *n = kmalloc(sizeof(struct vfs_node));
    if (!n) {
        printk(MODULE "node alloc failed\n");
        return NULL;
    }
    memset(n, 0, sizeof(*n));
    n->heap_alloc = true;
    return n;
}

void vfs_free_node(struct vfs_node *n)
{
    if (!n) return;
    bool was_heap = n->heap_alloc;
    memset(n, 0, sizeof(*n));
    if (was_heap) {
        kfree(n);
        return;
    }
    n->sibling     = node_free_list;
    node_free_list = n;
}

static uint32_t next_inode = 1000;

static struct vfs_node *vfs_root = NULL;

void vfs_set_root(struct vfs_node *root)
{
    vfs_root = root;
}

struct vfs_mount {
    char             path[VFS_PATH_MAX];
    char             fstype[16];
    struct vfs_node *root;      /* NULL for dynamic mounts tracked via mounted_root */
    struct vfs_node *mountpoint; /* the dir node that hosts this mount */
};

static struct vfs_mount mount_table[VFS_MOUNT_MAX];
static int              mount_count = 0;

struct vfs_fs_entry {
    char          fstype[16];
    struct fs_ops *ops;
};

static struct vfs_fs_entry fs_registry[VFS_FS_MAX];
static int                 fs_registry_count = 0;

void vfs_fs_register(const char *fstype, struct fs_ops *ops)
{
    if (fs_registry_count >= VFS_FS_MAX) {
        printk(MODULE "fs registry full\n");
        return;
    }
    struct vfs_fs_entry *e = &fs_registry[fs_registry_count++];
    strncpy(e->fstype, fstype, sizeof(e->fstype) - 1);
    e->fstype[sizeof(e->fstype) - 1] = '\0';
    e->ops = ops;
}

static struct fs_ops *fs_lookup_ops(const char *fstype)
{
    for (int i = 0; i < fs_registry_count; i++) {
        if (strcmp(fs_registry[i].fstype, fstype) == 0)
            return fs_registry[i].ops;
    }
    return NULL;
}

void vfs_mount(const char *path, const char *fstype, struct vfs_node *root)
{
    if (mount_count >= VFS_MOUNT_MAX) {
        printk(MODULE "mount table full\n");
        return;
    }
    struct vfs_mount *m = &mount_table[mount_count++];
    strncpy(m->path,   path,   VFS_PATH_MAX - 1);
    strncpy(m->fstype, fstype, sizeof(m->fstype) - 1);
    m->path[VFS_PATH_MAX - 1]        = '\0';
    m->fstype[sizeof(m->fstype) - 1] = '\0';
    m->root       = root;
    m->mountpoint = NULL;
}

int vfs_do_mount(const char *path, const char *fstype, void *device)
{
    struct fs_ops *ops = fs_lookup_ops(fstype);
    if (!ops) {
        printk(MODULE "unknown fstype: %s\n", fstype);
        return -1;
    }

    struct vfs_node *mp = vfs_lookup(path);
    if (!mp || !(mp->flags & VFS_DIR)) {
        printk(MODULE "mount: %s is not a directory\n", path);
        return -1;
    }
    if (mp->mounted_root) {
        printk(MODULE "mount: %s already has a mount\n", path);
        return -1;
    }
    if (mount_count >= VFS_MOUNT_MAX) {
        printk(MODULE "mount table full\n");
        return -1;
    }

    if (ops->mount(mp, device) != 0)
        return -1;

    struct vfs_mount *m = &mount_table[mount_count++];
    strncpy(m->path,   path,   VFS_PATH_MAX - 1);
    strncpy(m->fstype, fstype, sizeof(m->fstype) - 1);
    m->path[VFS_PATH_MAX - 1]        = '\0';
    m->fstype[sizeof(m->fstype) - 1] = '\0';
    m->root       = mp->mounted_root;
    m->mountpoint = mp;
    return 0;
}

int vfs_do_umount(const char *path)
{
    /* find mount table entry */
    int idx = -1;
    for (int i = 0; i < mount_count; i++) {
        if (mount_table[i].mountpoint &&
            strcmp(mount_table[i].path, path) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -1;

    /* Refuse if any other dynamic mount is nested under this path */
    uint32_t plen = (uint32_t)strlen(path);
    for (int i = 0; i < mount_count; i++) {
        if (i == idx || !mount_table[i].mountpoint) continue;
        const char *mp_path = mount_table[i].path;
        if (strncmp(mp_path, path, plen) == 0 &&
            (mp_path[plen] == '/' || mp_path[plen] == '\0') &&
            mp_path != path) {
            printk(MODULE "umount: %s has mounts below it\n", path);
            return -1;
        }
    }

    struct vfs_node *mp = mount_table[idx].mountpoint;
    if (!mp || !mp->mounted_root) return -1;

    struct fs_ops *ops = mp->mounted_root->fs;
    if (!ops || !ops->umount) return -1;

    if (ops->umount(mp) != 0)
        return -1;

    /* remove entry from mount table by shifting */
    for (int i = idx; i < mount_count - 1; i++)
        mount_table[i] = mount_table[i + 1];
    mount_count--;
    return 0;
}

int vfs_get_mounts(struct vfs_mount_info *out, int max)
{
    int n = mount_count < max ? mount_count : max;
    for (int i = 0; i < n; i++) {
        strncpy(out[i].path,   mount_table[i].path,   VFS_PATH_MAX - 1);
        strncpy(out[i].fstype, mount_table[i].fstype, 15);
        out[i].path[VFS_PATH_MAX - 1] = '\0';
        out[i].fstype[15]             = '\0';
        out[i].readonly = mount_table[i].root ? mount_table[i].root->readonly : false;
    }
    return n;
}

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
    if (cur->mounted_root) cur = cur->mounted_root;

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

        /* follow mount point if this dir has a filesystem mounted on it */
        if (cur->mounted_root) cur = cur->mounted_root;

        p = end;
    }
    return cur;
}

/* Resolve path relative to cwd into out_buf (VFS_PATH_MAX bytes).
 * Absolute paths are used as-is. Relative paths are appended to cwd.
 * Normalises: collapses double slashes, handles . and .. components.
 * Returns out_buf on success, NULL if the result would overflow. */
const char *vfs_resolve(const char *cwd, const char *path, char *out_buf)
{
    /* Components stack: store pointers into raw[] for each path segment */
    static char raw[VFS_PATH_MAX];
    uint32_t ri = 0;

    /* Build un-normalised absolute path in raw[] */
    if (path[0] == '/') {
        while (path[ri] && ri < VFS_PATH_MAX - 1) { raw[ri] = path[ri]; ri++; }
        raw[ri] = '\0';
    } else {
        uint32_t ci = 0;
        while (cwd[ci] && ri < VFS_PATH_MAX - 1) raw[ri++] = cwd[ci++];
        if (ri > 1 && raw[ri-1] != '/' && ri < VFS_PATH_MAX - 1) raw[ri++] = '/';
        uint32_t pi = 0;
        while (path[pi] && ri < VFS_PATH_MAX - 1) raw[ri++] = path[pi++];
        raw[ri] = '\0';
    }

    /* Walk components, track stack of kept segment bounds */
    const char *seg_start[64];
    uint32_t    seg_len[64];
    uint32_t    nseg = 0;

    const char *p = raw + 1; /* skip leading '/' */
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *end = p;
        while (*end && *end != '/') end++;
        uint32_t len = (uint32_t)(end - p);

        if (len == 1 && p[0] == '.') {
            /* skip */
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (nseg > 0) nseg--;
        } else {
            if (nseg < 64) { seg_start[nseg] = p; seg_len[nseg] = len; nseg++; }
        }
        p = end;
    }

    /* Emit result */
    out_buf[0] = '/';
    uint32_t oi = 1;
    for (uint32_t s = 0; s < nseg; s++) {
        if (s > 0) { if (oi < VFS_PATH_MAX - 1) out_buf[oi++] = '/'; }
        for (uint32_t i = 0; i < seg_len[s]; i++) {
            if (oi < VFS_PATH_MAX - 1) out_buf[oi++] = seg_start[s][i];
            else return NULL;
        }
    }
    out_buf[oi] = '\0';
    return out_buf;
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
    if (node->parent && node->parent->readonly) return 0;

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
    if (parent->readonly) return NULL;

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
    n->inode    = next_inode++;
    n->flags    = VFS_FILE;
    n->writable = true;
    vfs_add_child(parent, n);
    return n;
}

/* ── vfs_mkdir ────────────────────────────────────────────────────────────── */
struct vfs_node *vfs_mkdir(const char *path)
{
    if (!vfs_root || !path || path[0] != '/') return NULL;

    const char *base = path;
    for (const char *s = path; *s; s++)
        if (*s == '/') base = s + 1;
    if (*base == '\0') return NULL;

    uint32_t parent_len = (uint32_t)(base - path);
    if (parent_len > 1) parent_len--;

    struct vfs_node *parent;
    if (parent_len == 0) {
        parent = vfs_root;
    } else {
        char parent_path[VFS_PATH_MAX];
        if (parent_len >= VFS_PATH_MAX) return NULL;
        memcpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';
        parent = vfs_lookup(parent_path);
    }
    if (!parent || !(parent->flags & VFS_DIR)) return NULL;
    if (parent->readonly) return NULL;

    uint32_t blen = (uint32_t)strlen(base);
    if (dir_child(parent, base, blen)) return NULL; /* already exists */

    struct vfs_node *n = vfs_alloc_node();
    if (!n) return NULL;
    if (blen >= VFS_NAME_MAX) blen = VFS_NAME_MAX - 1;
    memcpy(n->name, base, blen);
    n->name[blen] = '\0';
    n->inode = next_inode++;
    n->flags = VFS_DIR;
    vfs_add_child(parent, n);
    return n;
}

/* ── vfs_unlink ───────────────────────────────────────────────────────────── */
int vfs_unlink(const char *path)
{
    struct vfs_node *n = vfs_lookup(path);
    if (!n) return -1;
    if (n->flags & VFS_DIR) return -1; /* use rmdir, not unlink */
    if (n->flags & VFS_CHARDEV) return -1; /* don't remove device nodes */

    struct vfs_node *parent = n->parent;
    if (!parent) return -1;
    if (parent->readonly) return -1;

    /* Unlink from parent's children list */
    if (parent->children == n) {
        parent->children = n->sibling;
    } else {
        struct vfs_node *s = parent->children;
        while (s && s->sibling != n) s = s->sibling;
        if (!s) return -1;
        s->sibling = n->sibling;
    }

    /* Free heap buffer if writable */
    if (n->writable && n->buf_cap > 0)
        kfree((void *)n->data);

    vfs_free_node(n);
    return 0;
}

/* Detach child from parent's children list without freeing it. */
static void vfs_detach_child(struct vfs_node *parent, struct vfs_node *child)
{
    if (parent->children == child) {
        parent->children = child->sibling;
    } else {
        struct vfs_node *s = parent->children;
        while (s && s->sibling != child) s = s->sibling;
        if (s) s->sibling = child->sibling;
    }
    child->sibling = NULL;
    child->parent  = NULL;
}

/* ── vfs_rename ───────────────────────────────────────────────────────────── */
int vfs_rename(const char *old_path, const char *new_path)
{
    if (!old_path || !new_path) return -1;

    struct vfs_node *old_node = vfs_lookup(old_path);
    if (!old_node) return -1;

    /* Refuse to rename root */
    if (!old_node->parent || old_node->parent == old_node) return -1;

    /* Same path — no-op */
    if (strcmp(old_path, new_path) == 0) return 0;

    /* Refuse to move a directory into itself */
    uint32_t old_len = (uint32_t)strlen(old_path);
    if ((old_node->flags & VFS_DIR) &&
        strncmp(new_path, old_path, old_len) == 0 &&
        new_path[old_len] == '/') {
        return -1;
    }

    /* Derive new parent path and new basename */
    const char *new_base = new_path;
    for (const char *s = new_path; *s; s++)
        if (*s == '/') new_base = s + 1;
    if (*new_base == '\0') return -1;

    uint32_t new_parent_len = (uint32_t)(new_base - new_path);
    if (new_parent_len > 1) new_parent_len--; /* strip trailing slash */

    struct vfs_node *new_parent;
    if (new_parent_len == 0) {
        new_parent = vfs_root;
    } else {
        char new_parent_path[VFS_PATH_MAX];
        if (new_parent_len >= VFS_PATH_MAX) return -1;
        memcpy(new_parent_path, new_path, new_parent_len);
        new_parent_path[new_parent_len] = '\0';
        new_parent = vfs_lookup(new_parent_path);
    }
    if (!new_parent || !(new_parent->flags & VFS_DIR)) return -1;
    if (new_parent->readonly) return -1;
    if (old_node->parent->readonly) return -1;

    struct vfs_node *new_node = vfs_lookup(new_path);

    if (new_node) {
        /* Refuse to overwrite device nodes */
        if (new_node->flags & VFS_CHARDEV) return -1;

        /* Type mismatch: file over dir or dir over file */
        bool old_is_dir = !!(old_node->flags & VFS_DIR);
        bool new_is_dir = !!(new_node->flags & VFS_DIR);
        if (old_is_dir != new_is_dir) return -1;

        /* Directory over directory: target must be empty */
        if (new_is_dir && new_node->children) return -1;

        /* Detach and free the target */
        vfs_detach_child(new_parent, new_node);
        if (new_node->writable && new_node->buf_cap > 0)
            kfree((void *)new_node->data);
        vfs_free_node(new_node);
    }

    /* Detach old node from its current parent, re-parent under new_parent */
    vfs_detach_child(old_node->parent, old_node);

    uint32_t blen = (uint32_t)strlen(new_base);
    if (blen >= VFS_NAME_MAX) blen = VFS_NAME_MAX - 1;
    memcpy(old_node->name, new_base, blen);
    old_node->name[blen] = '\0';

    vfs_add_child(new_parent, old_node);
    return 0;
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
    if (vfs_root && vfs_root->mounted_root)
        return vfs_root->mounted_root;
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
    out->size  = n->size;
    out->type  = n->flags;
    out->inode = n->inode;
    return 0;
}

int vfs_fstat(struct vfs_node *node, struct vfs_stat *out)
{
    if (!node) return -1;
    out->size  = node->size;
    out->type  = node->flags;
    out->inode = node->inode;
    return 0;
}

/* ── ioctl ───────────────────────────────────────────────────────────────── */
int32_t vfs_ioctl(struct vfs_node *node, uint32_t request, void *arg)
{
    if (!node || !node->control_op) return -1;
    return node->control_op(node, request, arg);
}

/* ── vfs_register_dev ────────────────────────────────────────────────────── */
struct vfs_node *vfs_register_dev(const char *name, uint32_t inode,
    uint32_t (*read_op)(uint32_t, uint32_t, void *),
    uint32_t (*write_op)(uint32_t, uint32_t, const void *),
    int32_t  (*control_op)(struct vfs_node *, uint32_t, void *))
{
    struct vfs_node *dev_dir = vfs_lookup("/dev");
    if (!dev_dir)
        return NULL;

    struct vfs_node *n = vfs_alloc_node();
    if (!n) {
        printk(MODULE "vfs_register_dev: node pool exhausted\n");
        return NULL;
    }

    uint32_t nlen = (uint32_t)strlen(name);
    if (nlen >= VFS_NAME_MAX) nlen = VFS_NAME_MAX - 1;
    memcpy(n->name, name, nlen);
    n->name[nlen] = '\0';
    n->flags      = VFS_FILE | VFS_CHARDEV;
    n->inode      = inode ? inode : next_inode++;
    n->read_op    = read_op;
    n->write_op   = write_op;
    n->control_op = control_op;
    vfs_add_child(dev_dir, n);
    return n;
}
