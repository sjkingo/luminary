/* tmpfs.c — in-memory writable filesystem driver.
 *
 * Implements struct fs_ops for "tmpfs". Mount creates a fresh VFS_DIR root
 * node (no block device needed). Files in the subtree use the standard
 * heap-backed writable node mechanism already in vfs.c.
 *
 * On umount, all nodes in the subtree are freed recursively and the
 * mountpoint's mounted_root is cleared.
 */

#include <string.h>
#include "kernel/vfs.h"
#include "kernel/heap.h"
#include "kernel/kernel.h"

/* forward declaration so tmpfs_mount can reference it */
static struct fs_ops tmpfs_ops;

static void tmpfs_free_tree(struct vfs_node *n)
{
    if (!n) return;
    struct vfs_node *c = n->children;
    while (c) {
        struct vfs_node *next = c->sibling;
        tmpfs_free_tree(c);
        c = next;
    }
    if (n->writable && n->buf_cap > 0)
        kfree((void *)n->data);
    vfs_free_node(n);
}

static int tmpfs_mount(struct vfs_node *mountpoint)
{
    struct vfs_node *root = vfs_alloc_node();
    if (!root) return -1;
    strncpy(root->name, mountpoint->name, VFS_NAME_MAX - 1);
    root->flags    = VFS_DIR;
    root->readonly = false;
    root->parent   = mountpoint->parent;
    root->fs       = &tmpfs_ops;

    mountpoint->mounted_root = root;
    return 0;
}

static int tmpfs_umount(struct vfs_node *mountpoint)
{
    struct vfs_node *root = mountpoint->mounted_root;
    if (!root) return -1;

    tmpfs_free_tree(root);
    mountpoint->mounted_root = NULL;
    return 0;
}

static struct fs_ops tmpfs_ops = {
    .mount  = tmpfs_mount,
    .umount = tmpfs_umount,
};

void init_tmpfs(void)
{
    vfs_fs_register("tmpfs", &tmpfs_ops);
}
