/* initrd: cpio "newc" (SVR4 without CRC) filesystem driver.
 *
 * Parses the cpio archive passed as a multiboot module, builds a VFS tree,
 * and exposes it as a registered "initrd" filesystem driver. Mounts are
 * zero-copy: file data pointers point directly into the cpio buffer.
 *
 * cpio newc format per entry:
 *   - 110-byte ASCII header
 *   - filename (nul-terminated, padded to 4-byte boundary after header+name)
 *   - file data (padded to 4-byte boundary)
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "kernel/initrd.h"
#include "kernel/vfs.h"
#include "kernel/kernel.h"

#define MODULE "initrd: "

#define CPIO_MAGIC "070701"
#define CPIO_MAGIC_LEN 6
#define CPIO_HDR_SIZE 110

struct cpio_hdr {
    char c_magic[6];
    char c_ino[8];
    char c_mode[8];
    char c_uid[8];
    char c_gid[8];
    char c_nlink[8];
    char c_mtime[8];
    char c_filesize[8];
    char c_devmajor[8];
    char c_devminor[8];
    char c_rdevmajor[8];
    char c_rdevminor[8];
    char c_namesize[8];
    char c_check[8];
};

static uint32_t hex8(const char *s)
{
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        char c = s[i];
        uint32_t d;
        if      (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else d = 0;
        v = (v << 4) | d;
    }
    return v;
}

static uint32_t align4(uint32_t x) { return (x + 3) & ~3u; }

static struct vfs_node *ensure_dir(struct vfs_node *root, const char *path)
{
    struct vfs_node *cur = root;
    const char *p = path;

    while (*p == '/') p++;

    while (*p) {
        const char *end = p;
        while (*end && *end != '/') end++;
        uint32_t len = (uint32_t)(end - p);

        if (!*end) break;

        struct vfs_node *child = NULL;
        struct vfs_node *c = cur->children;
        while (c) {
            if (strlen(c->name) == len && memcmp(c->name, p, len) == 0) {
                child = c; break;
            }
            c = c->sibling;
        }

        if (!child) {
            child = vfs_alloc_node();
            if (!child) return NULL;
            memcpy(child->name, p, len);
            child->name[len] = '\0';
            child->flags  = VFS_DIR;
            child->parent = cur;
            child->sibling = cur->children;
            cur->children = child;
        }
        cur = child;
        p = end;
        while (*p == '/') p++;
    }
    return cur;
}

static struct vfs_node *find_or_create(struct vfs_node *parent, const char *name,
                                        uint8_t flags)
{
    struct vfs_node *c = parent->children;
    while (c) {
        if (strcmp(c->name, name) == 0) return c;
        c = c->sibling;
    }
    struct vfs_node *n = vfs_alloc_node();
    if (!n) return NULL;
    uint32_t namelen = (uint32_t)strlen(name);
    if (namelen >= VFS_NAME_MAX) namelen = VFS_NAME_MAX - 1;
    memcpy(n->name, name, namelen);
    n->name[namelen] = '\0';
    n->flags  = flags;
    n->parent = parent;
    n->sibling = parent->children;
    parent->children = n;
    return n;
}

static struct vfs_node *initrd_parse(const void *data, uint32_t size,
                                     uint32_t *file_count_out)
{
    struct vfs_node *root = vfs_alloc_node();
    if (!root) panic("initrd: out of VFS nodes");
    root->name[0] = '/';
    root->name[1] = '\0';
    root->flags   = VFS_DIR;
    root->parent  = root;

    const uint8_t *base  = (const uint8_t *)data;
    const uint8_t *end   = base + size;
    const uint8_t *cur   = base;
    uint32_t file_count  = 0;

    while (cur + CPIO_HDR_SIZE <= end) {
        const struct cpio_hdr *hdr = (const struct cpio_hdr *)cur;

        if (memcmp(hdr->c_magic, CPIO_MAGIC, CPIO_MAGIC_LEN) != 0) {
            printk(MODULE "bad magic at offset 0x%lx, stopping\n",
                   (uint32_t)(cur - base));
            break;
        }

        uint32_t namesize = hex8(hdr->c_namesize);
        uint32_t filesize = hex8(hdr->c_filesize);

        const char *name = (const char *)(cur + CPIO_HDR_SIZE);

        if (namesize >= 11 && memcmp(name, "TRAILER!!!", 10) == 0)
            break;

        uint32_t data_offset = align4(CPIO_HDR_SIZE + namesize);
        const uint8_t *file_data = cur + data_offset;
        uint32_t next_offset     = data_offset + align4(filesize);

        if (cur + next_offset > end) {
            printk(MODULE "entry truncated, stopping\n");
            break;
        }

        if (namesize == 2 && name[0] == '.') {
            cur += next_offset;
            continue;
        }

        const char *entry_name = name;
        if (namesize > 2 && entry_name[0] == '.' && entry_name[1] == '/')
            entry_name += 2;

        uint32_t mode  = hex8(hdr->c_mode);
        bool is_dir    = (mode & 0170000) == 0040000;

        char fullpath[VFS_PATH_MAX];
        fullpath[0] = '/';
        uint32_t enlen = (uint32_t)strlen(entry_name);
        if (enlen >= VFS_PATH_MAX - 1) enlen = VFS_PATH_MAX - 2;
        memcpy(fullpath + 1, entry_name, enlen);
        fullpath[1 + enlen] = '\0';

        struct vfs_node *parent = ensure_dir(root, fullpath);
        if (!parent) {
            printk(MODULE "out of nodes, stopping\n");
            break;
        }

        const char *basename = entry_name;
        for (const char *s = entry_name; *s; s++)
            if (*s == '/') basename = s + 1;

        if (is_dir) {
            struct vfs_node *n = find_or_create(parent, basename, VFS_DIR);
            if (n) n->inode = hex8(hdr->c_ino);
        } else {
            struct vfs_node *n = find_or_create(parent, basename, VFS_FILE);
            if (n) {
                n->inode = hex8(hdr->c_ino);
                n->size  = filesize;
                n->data  = file_data;
                file_count++;
            }
        }

        cur += next_offset;
    }

    if (file_count_out) *file_count_out = file_count;
    return root;
}

static const void *initrd_data = NULL;
static uint32_t    initrd_data_size = 0;

static struct fs_ops initrd_ops;

static void initrd_free_tree(struct vfs_node *n)
{
    if (!n) return;
    struct vfs_node *c = n->children;
    while (c) {
        struct vfs_node *next = c->sibling;
        initrd_free_tree(c);
        c = next;
    }
    /* do not free n->data: zero-copy into multiboot module memory */
    vfs_free_node(n);
}

static int initrd_mount(struct vfs_node *mountpoint)
{
    if (!initrd_data) return -1;
    uint32_t file_count = 0;
    struct vfs_node *root = initrd_parse(initrd_data, initrd_data_size, &file_count);
    if (!root) return -1;
    root->readonly = true;
    root->fs       = &initrd_ops;
    mountpoint->mounted_root = root;
    DBGK("initrd: mounted %ld files\n", file_count);
    return 0;
}

static int initrd_umount(struct vfs_node *mountpoint)
{
    struct vfs_node *root = mountpoint->mounted_root;
    if (!root) return -1;
    initrd_free_tree(root);
    mountpoint->mounted_root = NULL;
    return 0;
}

static struct fs_ops initrd_ops = {
    .mount  = initrd_mount,
    .umount = initrd_umount,
};

void init_initrd(const void *data, uint32_t size)
{
    initrd_data      = data;
    initrd_data_size = size;
    vfs_fs_register("initrd", &initrd_ops);
}

const void *initrd_get_file(const char *path, uint32_t *size_out)
{
    struct vfs_node *n = vfs_lookup(path);
    if (!n || !(n->flags & VFS_FILE)) return NULL;
    *size_out = n->size;
    return n->data;
}
