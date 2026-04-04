/* initrd: cpio "newc" (SVR4 without CRC) parser.
 *
 * Parses the cpio archive passed as a multiboot module, builds a VFS tree,
 * and mounts it at "/". The VFS nodes point directly into the cpio data
 * (zero-copy for file contents).
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

/* ── cpio newc header (all fields are ASCII hex, no separators) ──────────── */
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

/* Parse an 8-char ASCII hex field. */
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

/* Round up to the nearest 4-byte boundary. */
static uint32_t align4(uint32_t x) { return (x + 3) & ~3u; }

/* ── VFS tree helpers ────────────────────────────────────────────────────── */

/* Ensure all intermediate directories exist for a path component, creating
 * them as needed under root. Returns the direct parent directory node. */
static struct vfs_node *ensure_dir(struct vfs_node *root, const char *path)
{
    /* path is a full absolute path like "/usr/bin/sh".
     * Walk each component; create dir nodes that don't exist yet. */
    struct vfs_node *cur = root;
    const char *p = path;

    /* skip leading '/' */
    while (*p == '/') p++;

    while (*p) {
        /* find next component */
        const char *end = p;
        while (*end && *end != '/') end++;
        uint32_t len = (uint32_t)(end - p);

        /* skip the last component — that's the file/dir itself */
        if (!*end) break;

        /* look for existing child */
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

/* Find or create a node named 'name' under parent dir. */
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

/* ── public entry point ──────────────────────────────────────────────────── */

uint32_t initrd_init(const void *data, uint32_t size)
{

    /* Create root directory node */
    struct vfs_node *root = vfs_alloc_node();
    if (!root) panic("initrd: out of VFS nodes");
    root->name[0] = '/';
    root->name[1] = '\0';
    root->flags   = VFS_DIR;
    root->parent  = root; /* root's parent is itself */

    const uint8_t *base  = (const uint8_t *)data;
    const uint8_t *end   = base + size;
    const uint8_t *cur   = base;
    uint32_t file_count  = 0;

    while (cur + CPIO_HDR_SIZE <= end) {
        const struct cpio_hdr *hdr = (const struct cpio_hdr *)cur;

        /* Validate magic */
        if (memcmp(hdr->c_magic, CPIO_MAGIC, CPIO_MAGIC_LEN) != 0) {
            printk(MODULE "bad magic at offset 0x%lx, stopping\n",
                   (uint32_t)(cur - base));
            break;
        }

        uint32_t namesize = hex8(hdr->c_namesize);
        uint32_t filesize = hex8(hdr->c_filesize);

        /* Filename immediately follows the header */
        const char *name = (const char *)(cur + CPIO_HDR_SIZE);

        /* Sentinel: "TRAILER!!!" marks end of archive */
        if (namesize >= 11 && memcmp(name, "TRAILER!!!", 10) == 0)
            break;

        /* File data follows name, both padded to 4-byte boundary from
         * the start of the header:
         *   header_end = cur + CPIO_HDR_SIZE
         *   name_end   = header_end + namesize  (includes NUL)
         *   data_start = align4(CPIO_HDR_SIZE + namesize) from cur */
        uint32_t data_offset = align4(CPIO_HDR_SIZE + namesize);
        const uint8_t *file_data = cur + data_offset;
        uint32_t next_offset     = data_offset + align4(filesize);

        if (cur + next_offset > end) {
            printk(MODULE "entry truncated, stopping\n");
            break;
        }

        /* Skip "." (the archive itself) */
        if (namesize == 2 && name[0] == '.') {
            cur += next_offset;
            continue;
        }

        /* Strip leading "./" that some tools add */
        const char *entry_name = name;
        if (namesize > 2 && entry_name[0] == '.' && entry_name[1] == '/')
            entry_name += 2;

        /* Determine type from cpio mode bits */
        uint32_t mode  = hex8(hdr->c_mode);
        bool is_dir    = (mode & 0170000) == 0040000;

        /* Find parent directory, creating intermediates as needed */
        /* Build a temporary path with a leading slash so ensure_dir works */
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

        /* Extract the final component name */
        const char *basename = entry_name;
        for (const char *s = entry_name; *s; s++)
            if (*s == '/') basename = s + 1;

        if (is_dir) {
            /* Ensure the directory node itself exists */
            find_or_create(parent, basename, VFS_DIR);
        } else {
            /* Regular file */
            struct vfs_node *n = find_or_create(parent, basename, VFS_FILE);
            if (n) {
                n->size = filesize;
                n->data = file_data;
                file_count++;
            }
        }

        cur += next_offset;
    }

    vfs_set_root(root);
    return file_count;
}

/* Look up a file by absolute path and return pointer + size.
 * Used by the kernel to load ELF binaries from the VFS. */
const void *initrd_get_file(const char *path, uint32_t *size_out)
{
    struct vfs_node *n = vfs_lookup(path);
    if (!n || !(n->flags & VFS_FILE)) return NULL;
    *size_out = n->size;
    return n->data;
}
