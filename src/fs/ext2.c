/* ext2.c — ext2 read-write filesystem driver.
 *
 * Implements struct fs_ops for "ext2". On mount, reads the superblock and
 * block group descriptors, then eagerly walks the directory tree from the
 * root inode (inode 2), populating the VFS tree with vfs_alloc_node nodes.
 *
 * File data is read on demand via per-slot read_op/write_op functions.
 * The slot table (EXT2_FILE_SLOTS entries) caches each open file's inode
 * using the same per-slot macro pattern as blkdev.c and pipe.c.
 *
 * Write support: read-modify-write on already-allocated blocks only.
 * No block allocation, no bitmap updates.
 *
 * On umount, all VFS nodes in the subtree are freed and the slot table
 * is cleared.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "fs/ext2.h"
#include "kernel/vfs.h"
#include "kernel/heap.h"
#include "kernel/kernel.h"
#include "drivers/blkdev.h"

#define MODULE "ext2: "

#define EXT2_MAGIC      0xEF53
#define EXT2_ROOT_INO   2
#define EXT2_IMODE_DIR  0x4000
#define EXT2_IMODE_FILE 0x8000
#define EXT2_IMODE_MASK 0xF000

struct ext2_superblock {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    /* extended superblock fields (rev >= 1) */
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    /* remainder unused */
} __attribute__((packed));

struct ext2_bgd {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} __attribute__((packed));

struct ext2_inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __attribute__((packed));

struct ext2_dirent {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[255];
} __attribute__((packed));

#define EXT2_FILE_SLOTS 32

struct ext2_slot {
    bool            used;
    uint32_t        ino;
    struct ext2_inode inode;
    struct vfs_node *node;  /* back-pointer to keep node->size in sync */
};

static struct blkdev   *g_dev;
static uint32_t         g_block_size;
static uint32_t         g_inodes_per_group;
static uint32_t         g_inode_size;
static uint32_t         g_bg_count;
static struct ext2_bgd *g_bgds;

static struct ext2_slot g_slots[EXT2_FILE_SLOTS];

static struct fs_ops ext2_ops;

static uint32_t ext2_alloc_block(void);
static void     ext2_free_block(uint32_t block_no);

static void ext2_read_block(uint32_t block_no, void *buf)
{
    uint32_t byte_off = block_no * g_block_size;
    uint32_t lba      = byte_off / BLKDEV_SECTOR_SIZE;
    uint32_t nsects   = g_block_size / BLKDEV_SECTOR_SIZE;
    g_dev->read_sectors(g_dev, lba, nsects, buf);
}

static void ext2_write_block(uint32_t block_no, const void *buf)
{
    uint32_t byte_off = block_no * g_block_size;
    uint32_t lba      = byte_off / BLKDEV_SECTOR_SIZE;
    uint32_t nsects   = g_block_size / BLKDEV_SECTOR_SIZE;
    g_dev->write_sectors(g_dev, lba, nsects, buf);
}

static void ext2_read_inode(uint32_t ino, struct ext2_inode *out)
{
    uint32_t group   = (ino - 1) / g_inodes_per_group;
    uint32_t idx     = (ino - 1) % g_inodes_per_group;
    uint32_t byte_off = g_bgds[group].bg_inode_table * g_block_size
                        + idx * g_inode_size;
    uint32_t lba     = byte_off / BLKDEV_SECTOR_SIZE;
    uint32_t off_in  = byte_off % BLKDEV_SECTOR_SIZE;

    /* Read enough sectors to cover the inode (may straddle a sector boundary) */
    uint8_t buf[2 * BLKDEV_SECTOR_SIZE];
    uint32_t nsects = (off_in + g_inode_size + BLKDEV_SECTOR_SIZE - 1)
                      / BLKDEV_SECTOR_SIZE;
    if (nsects > 2) nsects = 2;
    g_dev->read_sectors(g_dev, lba, nsects, buf);
    memcpy(out, buf + off_in, sizeof(struct ext2_inode));
}

static void ext2_write_inode(uint32_t ino, const struct ext2_inode *in)
{
    uint32_t group   = (ino - 1) / g_inodes_per_group;
    uint32_t idx     = (ino - 1) % g_inodes_per_group;
    uint32_t byte_off = g_bgds[group].bg_inode_table * g_block_size
                        + idx * g_inode_size;
    uint32_t lba     = byte_off / BLKDEV_SECTOR_SIZE;
    uint32_t off_in  = byte_off % BLKDEV_SECTOR_SIZE;

    uint8_t buf[2 * BLKDEV_SECTOR_SIZE];
    uint32_t nsects = (off_in + g_inode_size + BLKDEV_SECTOR_SIZE - 1)
                      / BLKDEV_SECTOR_SIZE;
    if (nsects > 2) nsects = 2;
    g_dev->read_sectors(g_dev, lba, nsects, buf);
    memcpy(buf + off_in, in, sizeof(struct ext2_inode));
    g_dev->write_sectors(g_dev, lba, nsects, buf);
}

/* Resolve logical block index within a file to physical block number.
 * Returns 0 for holes / unallocated blocks. */
static uint32_t ext2_file_block(const struct ext2_inode *inode, uint32_t blk_idx)
{
    uint32_t ptrs_per_block = g_block_size / 4;

    if (blk_idx < 12)
        return inode->i_block[blk_idx];

    blk_idx -= 12;

    if (blk_idx < ptrs_per_block) {
        if (!inode->i_block[12]) return 0;
        uint32_t *ibuf = kmalloc(g_block_size);
        if (!ibuf) return 0;
        ext2_read_block(inode->i_block[12], ibuf);
        uint32_t phys = ibuf[blk_idx];
        kfree(ibuf);
        return phys;
    }

    blk_idx -= ptrs_per_block;

    if (blk_idx < ptrs_per_block * ptrs_per_block) {
        if (!inode->i_block[13]) return 0;
        uint32_t *ibuf = kmalloc(g_block_size);
        if (!ibuf) return 0;
        ext2_read_block(inode->i_block[13], ibuf);
        uint32_t l1 = ibuf[blk_idx / ptrs_per_block];
        if (!l1) { kfree(ibuf); return 0; }
        ext2_read_block(l1, ibuf);
        uint32_t phys = ibuf[blk_idx % ptrs_per_block];
        kfree(ibuf);
        return phys;
    }

    /* triple indirect not implemented */
    return 0;
}

static int ext2_alloc_slot(uint32_t ino, const struct ext2_inode *inode,
                           struct vfs_node *node)
{
    for (int i = 0; i < EXT2_FILE_SLOTS; i++) {
        if (!g_slots[i].used) {
            g_slots[i].used  = true;
            g_slots[i].ino   = ino;
            g_slots[i].inode = *inode;
            g_slots[i].node  = node;
            return i;
        }
    }
    return -1;
}

static uint32_t ext2_do_read(int slot, uint32_t off, uint32_t len, void *buf)
{
    struct ext2_slot *s = &g_slots[slot];
    uint32_t file_size  = s->inode.i_size;

    if (off >= file_size) return 0;
    if (off + len > file_size) len = file_size - off;

    uint8_t *bbuf    = kmalloc(g_block_size);
    if (!bbuf) return 0;

    uint32_t done = 0;
    while (done < len) {
        uint32_t blk_idx  = (off + done) / g_block_size;
        uint32_t blk_off  = (off + done) % g_block_size;
        uint32_t to_copy  = g_block_size - blk_off;
        if (to_copy > len - done) to_copy = len - done;

        uint32_t phys = ext2_file_block(&s->inode, blk_idx);
        if (!phys) break;

        ext2_read_block(phys, bbuf);
        memcpy((uint8_t *)buf + done, bbuf + blk_off, to_copy);
        done += to_copy;
    }

    kfree(bbuf);
    return done;
}

static uint32_t ext2_do_write(int slot, uint32_t off, uint32_t len, const void *buf)
{
    struct ext2_slot *s = &g_slots[slot];
    uint8_t *bbuf = kmalloc(g_block_size);
    if (!bbuf) return 0;

    uint32_t done = 0;
    while (done < len) {
        uint32_t blk_idx = (off + done) / g_block_size;
        uint32_t blk_off = (off + done) % g_block_size;
        uint32_t to_copy = g_block_size - blk_off;
        if (to_copy > len - done) to_copy = len - done;

        uint32_t phys = ext2_file_block(&s->inode, blk_idx);
        if (!phys) {
            if (blk_idx >= 12) break; /* indirect allocation not implemented */
            phys = ext2_alloc_block();
            if (!phys) break;
            uint8_t *zbuf = kmalloc(g_block_size);
            if (!zbuf) { ext2_free_block(phys); break; }
            memset(zbuf, 0, g_block_size);
            ext2_write_block(phys, zbuf);
            kfree(zbuf);
            s->inode.i_block[blk_idx] = phys;
            s->inode.i_blocks += g_block_size / BLKDEV_SECTOR_SIZE;
        }

        ext2_read_block(phys, bbuf);
        memcpy(bbuf + blk_off, (const uint8_t *)buf + done, to_copy);
        ext2_write_block(phys, bbuf);
        done += to_copy;
    }

    kfree(bbuf);

    if (done > 0 && off + done > s->inode.i_size) {
        s->inode.i_size = off + done;
        ext2_write_inode(s->ino, &s->inode);
        if (s->node) s->node->size = s->inode.i_size;
    }

    return done;
}

/* Per-slot op stubs — same pattern as blkdev.c/pipe.c */
#define EXT2_SLOT(i) \
    static uint32_t ext2_read_op_##i(uint32_t o, uint32_t l, void *b) \
        { return ext2_do_read(i, o, l, b); } \
    static uint32_t ext2_write_op_##i(uint32_t o, uint32_t l, const void *b) \
        { return ext2_do_write(i, o, l, b); }

EXT2_SLOT(0)  EXT2_SLOT(1)  EXT2_SLOT(2)  EXT2_SLOT(3)
EXT2_SLOT(4)  EXT2_SLOT(5)  EXT2_SLOT(6)  EXT2_SLOT(7)
EXT2_SLOT(8)  EXT2_SLOT(9)  EXT2_SLOT(10) EXT2_SLOT(11)
EXT2_SLOT(12) EXT2_SLOT(13) EXT2_SLOT(14) EXT2_SLOT(15)
EXT2_SLOT(16) EXT2_SLOT(17) EXT2_SLOT(18) EXT2_SLOT(19)
EXT2_SLOT(20) EXT2_SLOT(21) EXT2_SLOT(22) EXT2_SLOT(23)
EXT2_SLOT(24) EXT2_SLOT(25) EXT2_SLOT(26) EXT2_SLOT(27)
EXT2_SLOT(28) EXT2_SLOT(29) EXT2_SLOT(30) EXT2_SLOT(31)

typedef uint32_t (*read_op_t)(uint32_t, uint32_t, void *);
typedef uint32_t (*write_op_t)(uint32_t, uint32_t, const void *);

static const read_op_t g_read_ops[EXT2_FILE_SLOTS] = {
    ext2_read_op_0,  ext2_read_op_1,  ext2_read_op_2,  ext2_read_op_3,
    ext2_read_op_4,  ext2_read_op_5,  ext2_read_op_6,  ext2_read_op_7,
    ext2_read_op_8,  ext2_read_op_9,  ext2_read_op_10, ext2_read_op_11,
    ext2_read_op_12, ext2_read_op_13, ext2_read_op_14, ext2_read_op_15,
    ext2_read_op_16, ext2_read_op_17, ext2_read_op_18, ext2_read_op_19,
    ext2_read_op_20, ext2_read_op_21, ext2_read_op_22, ext2_read_op_23,
    ext2_read_op_24, ext2_read_op_25, ext2_read_op_26, ext2_read_op_27,
    ext2_read_op_28, ext2_read_op_29, ext2_read_op_30, ext2_read_op_31,
};

static const write_op_t g_write_ops[EXT2_FILE_SLOTS] = {
    ext2_write_op_0,  ext2_write_op_1,  ext2_write_op_2,  ext2_write_op_3,
    ext2_write_op_4,  ext2_write_op_5,  ext2_write_op_6,  ext2_write_op_7,
    ext2_write_op_8,  ext2_write_op_9,  ext2_write_op_10, ext2_write_op_11,
    ext2_write_op_12, ext2_write_op_13, ext2_write_op_14, ext2_write_op_15,
    ext2_write_op_16, ext2_write_op_17, ext2_write_op_18, ext2_write_op_19,
    ext2_write_op_20, ext2_write_op_21, ext2_write_op_22, ext2_write_op_23,
    ext2_write_op_24, ext2_write_op_25, ext2_write_op_26, ext2_write_op_27,
    ext2_write_op_28, ext2_write_op_29, ext2_write_op_30, ext2_write_op_31,
};

static void ext2_walk_dir(struct vfs_node *parent, uint32_t dir_ino);

static void ext2_add_file(struct vfs_node *parent, uint32_t ino,
                          const char *name, uint8_t namelen,
                          const struct ext2_inode *inode)
{
    uint16_t mode = inode->i_mode & EXT2_IMODE_MASK;

    struct vfs_node *node = vfs_alloc_node();
    if (!node) {
        printk(MODULE "out of VFS nodes\n");
        return;
    }

    uint32_t cplen = namelen < VFS_NAME_MAX - 1 ? namelen : VFS_NAME_MAX - 1;
    memcpy(node->name, name, cplen);
    node->name[cplen] = '\0';
    node->inode = ino;

    if (mode == EXT2_IMODE_DIR) {
        node->flags = VFS_DIR;
        node->fs    = &ext2_ops;
        vfs_add_child(parent, node);
        ext2_walk_dir(node, ino);
    } else if (mode == EXT2_IMODE_FILE) {
        int slot = ext2_alloc_slot(ino, inode, node);
        if (slot < 0) {
            printk(MODULE "out of file slots for inode %ld\n", ino);
            vfs_free_node(node);
            return;
        }
        node->flags    = VFS_FILE | VFS_CHARDEV;
        node->size     = inode->i_size;
        node->read_op  = g_read_ops[slot];
        node->write_op = g_write_ops[slot];
        node->fs       = &ext2_ops;
        vfs_add_child(parent, node);
    } else {
        /* symlinks, devices etc. — skip */
        vfs_free_node(node);
    }
}

static void ext2_walk_dir(struct vfs_node *parent, uint32_t dir_ino)
{
    struct ext2_inode inode;
    ext2_read_inode(dir_ino, &inode);

    uint8_t *bbuf = kmalloc(g_block_size);
    if (!bbuf) return;

    /* Iterate direct blocks */
    for (int bi = 0; bi < 12; bi++) {
        uint32_t phys = inode.i_block[bi];
        if (!phys) break;

        ext2_read_block(phys, bbuf);

        uint32_t pos = 0;
        while (pos < g_block_size) {
            struct ext2_dirent *de = (struct ext2_dirent *)(bbuf + pos);
            if (!de->rec_len) break;

            if (de->inode && de->name_len) {
                bool is_dot = (de->name_len == 1 && de->name[0] == '.');
                bool is_dotdot = (de->name_len == 2 &&
                                  de->name[0] == '.' && de->name[1] == '.');
                if (!is_dot && !is_dotdot) {
                    struct ext2_inode child_inode;
                    ext2_read_inode(de->inode, &child_inode);
                    ext2_add_file(parent, de->inode, de->name,
                                  de->name_len, &child_inode);
                }
            }

            pos += de->rec_len;
        }
    }

    /* Single indirect block */
    if (inode.i_block[12]) {
        uint32_t *ibuf = kmalloc(g_block_size);
        if (ibuf) {
            ext2_read_block(inode.i_block[12], ibuf);
            uint32_t ptrs = g_block_size / 4;
            for (uint32_t i = 0; i < ptrs; i++) {
                uint32_t phys = ibuf[i];
                if (!phys) break;

                ext2_read_block(phys, bbuf);

                uint32_t pos = 0;
                while (pos < g_block_size) {
                    struct ext2_dirent *de = (struct ext2_dirent *)(bbuf + pos);
                    if (!de->rec_len) break;

                    if (de->inode && de->name_len) {
                        bool is_dot = (de->name_len == 1 && de->name[0] == '.');
                        bool is_dotdot = (de->name_len == 2 &&
                                          de->name[0] == '.' && de->name[1] == '.');
                        if (!is_dot && !is_dotdot) {
                            struct ext2_inode child_inode;
                            ext2_read_inode(de->inode, &child_inode);
                            ext2_add_file(parent, de->inode, de->name,
                                          de->name_len, &child_inode);
                        }
                    }

                    pos += de->rec_len;
                }
            }
            kfree(ibuf);
        }
    }

    kfree(bbuf);
}

/* Allocate a free block, set its bit in the block bitmap, update BGD and
 * superblock free counts. Returns block number or 0 on failure. */
static uint32_t ext2_alloc_block(void)
{
    uint8_t *bitmap = kmalloc(g_block_size);
    if (!bitmap) return 0;

    /* Read superblock to update s_free_blocks_count */
    uint8_t sb_raw[2 * BLKDEV_SECTOR_SIZE];
    g_dev->read_sectors(g_dev, 2, 2, sb_raw);
    struct ext2_superblock *sb = (struct ext2_superblock *)sb_raw;

    for (uint32_t g = 0; g < g_bg_count; g++) {
        if (g_bgds[g].bg_free_blocks_count == 0) continue;

        ext2_read_block(g_bgds[g].bg_block_bitmap, bitmap);

        uint32_t blocks_in_group = sb->s_blocks_per_group;
        uint32_t remaining = sb->s_blocks_count - g * sb->s_blocks_per_group;
        if (remaining < blocks_in_group) blocks_in_group = remaining;

        for (uint32_t i = 0; i < blocks_in_group; i++) {
            if (!(bitmap[i / 8] & (1 << (i % 8)))) {
                bitmap[i / 8] |= (1 << (i % 8));
                ext2_write_block(g_bgds[g].bg_block_bitmap, bitmap);

                g_bgds[g].bg_free_blocks_count--;
                uint32_t bgdt_block = (g_block_size == 1024) ? 2 : 1;
                uint32_t bgdt_bytes = g_bg_count * sizeof(struct ext2_bgd);
                uint32_t bgdt_blks  = (bgdt_bytes + g_block_size - 1) / g_block_size;
                uint8_t *bgdt_buf = kmalloc(bgdt_blks * g_block_size);
                if (bgdt_buf) {
                    for (uint32_t b = 0; b < bgdt_blks; b++)
                        ext2_read_block(bgdt_block + b, bgdt_buf + b * g_block_size);
                    memcpy(bgdt_buf, g_bgds, bgdt_bytes);
                    for (uint32_t b = 0; b < bgdt_blks; b++)
                        ext2_write_block(bgdt_block + b, bgdt_buf + b * g_block_size);
                    kfree(bgdt_buf);
                }

                if (sb->s_free_blocks_count > 0) sb->s_free_blocks_count--;
                g_dev->write_sectors(g_dev, 2, 2, sb_raw);

                kfree(bitmap);
                return g * sb->s_blocks_per_group + i + sb->s_first_data_block;
            }
        }
    }

    kfree(bitmap);
    return 0;
}

/* Allocate a free inode, set its bit in the inode bitmap, update BGD and
 * superblock free counts. Returns inode number (1-based) or 0 on failure. */
static uint32_t ext2_alloc_inode(void)
{
    uint8_t *bitmap = kmalloc(g_block_size);
    if (!bitmap) return 0;

    uint8_t sb_raw[2 * BLKDEV_SECTOR_SIZE];
    g_dev->read_sectors(g_dev, 2, 2, sb_raw);
    struct ext2_superblock *sb = (struct ext2_superblock *)sb_raw;

    for (uint32_t g = 0; g < g_bg_count; g++) {
        if (g_bgds[g].bg_free_inodes_count == 0) continue;

        ext2_read_block(g_bgds[g].bg_inode_bitmap, bitmap);

        for (uint32_t i = 0; i < g_inodes_per_group; i++) {
            if (!(bitmap[i / 8] & (1 << (i % 8)))) {
                bitmap[i / 8] |= (1 << (i % 8));
                ext2_write_block(g_bgds[g].bg_inode_bitmap, bitmap);

                g_bgds[g].bg_free_inodes_count--;
                uint32_t bgdt_block = (g_block_size == 1024) ? 2 : 1;
                uint32_t bgdt_bytes = g_bg_count * sizeof(struct ext2_bgd);
                uint32_t bgdt_blks  = (bgdt_bytes + g_block_size - 1) / g_block_size;
                uint8_t *bgdt_buf = kmalloc(bgdt_blks * g_block_size);
                if (bgdt_buf) {
                    for (uint32_t b = 0; b < bgdt_blks; b++)
                        ext2_read_block(bgdt_block + b, bgdt_buf + b * g_block_size);
                    memcpy(bgdt_buf, g_bgds, bgdt_bytes);
                    for (uint32_t b = 0; b < bgdt_blks; b++)
                        ext2_write_block(bgdt_block + b, bgdt_buf + b * g_block_size);
                    kfree(bgdt_buf);
                }

                if (sb->s_free_inodes_count > 0) sb->s_free_inodes_count--;
                g_dev->write_sectors(g_dev, 2, 2, sb_raw);

                kfree(bitmap);
                return g * g_inodes_per_group + i + 1;
            }
        }
    }

    kfree(bitmap);
    return 0;
}

/* Free a block: clear its bit in the block bitmap, increment free counts. */
static void ext2_free_block(uint32_t block_no)
{
    if (!block_no) return;

    uint8_t sb_raw[2 * BLKDEV_SECTOR_SIZE];
    g_dev->read_sectors(g_dev, 2, 2, sb_raw);
    struct ext2_superblock *sb = (struct ext2_superblock *)sb_raw;

    uint32_t group = (block_no - sb->s_first_data_block) / sb->s_blocks_per_group;
    uint32_t idx   = (block_no - sb->s_first_data_block) % sb->s_blocks_per_group;
    if (group >= g_bg_count) return;

    uint8_t *bitmap = kmalloc(g_block_size);
    if (!bitmap) return;
    ext2_read_block(g_bgds[group].bg_block_bitmap, bitmap);
    bitmap[idx / 8] &= ~(1 << (idx % 8));
    ext2_write_block(g_bgds[group].bg_block_bitmap, bitmap);
    kfree(bitmap);

    g_bgds[group].bg_free_blocks_count++;
    uint32_t bgdt_block = (g_block_size == 1024) ? 2 : 1;
    uint32_t bgdt_bytes = g_bg_count * sizeof(struct ext2_bgd);
    uint32_t bgdt_blks  = (bgdt_bytes + g_block_size - 1) / g_block_size;
    uint8_t *bgdt_buf = kmalloc(bgdt_blks * g_block_size);
    if (bgdt_buf) {
        for (uint32_t b = 0; b < bgdt_blks; b++)
            ext2_read_block(bgdt_block + b, bgdt_buf + b * g_block_size);
        memcpy(bgdt_buf, g_bgds, bgdt_bytes);
        for (uint32_t b = 0; b < bgdt_blks; b++)
            ext2_write_block(bgdt_block + b, bgdt_buf + b * g_block_size);
        kfree(bgdt_buf);
    }

    sb->s_free_blocks_count++;
    g_dev->write_sectors(g_dev, 2, 2, sb_raw);
}

/* Free an inode: clear its bit in the inode bitmap, increment free counts. */
static void ext2_free_inode(uint32_t ino)
{
    if (!ino) return;

    uint8_t sb_raw[2 * BLKDEV_SECTOR_SIZE];
    g_dev->read_sectors(g_dev, 2, 2, sb_raw);
    struct ext2_superblock *sb = (struct ext2_superblock *)sb_raw;

    uint32_t group = (ino - 1) / g_inodes_per_group;
    uint32_t idx   = (ino - 1) % g_inodes_per_group;
    if (group >= g_bg_count) return;

    uint8_t *bitmap = kmalloc(g_block_size);
    if (!bitmap) return;
    ext2_read_block(g_bgds[group].bg_inode_bitmap, bitmap);
    bitmap[idx / 8] &= ~(1 << (idx % 8));
    ext2_write_block(g_bgds[group].bg_inode_bitmap, bitmap);
    kfree(bitmap);

    g_bgds[group].bg_free_inodes_count++;
    uint32_t bgdt_block = (g_block_size == 1024) ? 2 : 1;
    uint32_t bgdt_bytes = g_bg_count * sizeof(struct ext2_bgd);
    uint32_t bgdt_blks  = (bgdt_bytes + g_block_size - 1) / g_block_size;
    uint8_t *bgdt_buf = kmalloc(bgdt_blks * g_block_size);
    if (bgdt_buf) {
        for (uint32_t b = 0; b < bgdt_blks; b++)
            ext2_read_block(bgdt_block + b, bgdt_buf + b * g_block_size);
        memcpy(bgdt_buf, g_bgds, bgdt_bytes);
        for (uint32_t b = 0; b < bgdt_blks; b++)
            ext2_write_block(bgdt_block + b, bgdt_buf + b * g_block_size);
        kfree(bgdt_buf);
    }

    sb->s_free_inodes_count++;
    g_dev->write_sectors(g_dev, 2, 2, sb_raw);
}

/* Add a directory entry to dir_ino for (child_ino, name, file_type).
 * Tries to fit in padding of the last entry in each existing block first.
 * Allocates a new block if no space found. Returns 0 on success. */
static int ext2_dir_add_entry(uint32_t dir_ino, uint32_t child_ino,
                              const char *name, uint8_t file_type)
{
    uint8_t namelen = (uint8_t)strlen(name);
    uint32_t needed = (8 + namelen + 3) & ~3u; /* rec_len must be 4-byte aligned */

    struct ext2_inode dir_inode;
    ext2_read_inode(dir_ino, &dir_inode);

    uint8_t *bbuf = kmalloc(g_block_size);
    if (!bbuf) return -1;

    /* Walk direct blocks looking for a block with enough padding in last entry */
    for (int bi = 0; bi < 12; bi++) {
        uint32_t phys = dir_inode.i_block[bi];
        if (!phys) break;

        ext2_read_block(phys, bbuf);

        uint32_t pos = 0;
        struct ext2_dirent *last = NULL;
        while (pos < g_block_size) {
            struct ext2_dirent *de = (struct ext2_dirent *)(bbuf + pos);
            if (!de->rec_len) break;
            last = de;
            pos += de->rec_len;
        }

        if (last) {
            uint32_t actual = (8 + last->name_len + 3) & ~3u;
            uint32_t slack  = last->rec_len - actual;
            if (slack >= needed) {
                /* Shrink last entry and append new one in its slack */
                last->rec_len = (uint16_t)actual;
                struct ext2_dirent *ne = (struct ext2_dirent *)((uint8_t *)last + actual);
                ne->inode    = child_ino;
                ne->rec_len  = (uint16_t)slack;
                ne->name_len = namelen;
                ne->file_type = file_type;
                memcpy(ne->name, name, namelen);
                ext2_write_block(phys, bbuf);
                kfree(bbuf);
                return 0;
            }
        }
    }

    /* No space in existing blocks — allocate a new direct block */
    uint32_t new_block = ext2_alloc_block();
    if (!new_block) { kfree(bbuf); return -1; }

    /* Find the first empty direct block slot */
    int slot = -1;
    for (int bi = 0; bi < 12; bi++) {
        if (!dir_inode.i_block[bi]) { slot = bi; break; }
    }
    if (slot < 0) { ext2_free_block(new_block); kfree(bbuf); return -1; }

    memset(bbuf, 0, g_block_size);
    struct ext2_dirent *ne = (struct ext2_dirent *)bbuf;
    ne->inode     = child_ino;
    ne->rec_len   = (uint16_t)g_block_size;
    ne->name_len  = namelen;
    ne->file_type = file_type;
    memcpy(ne->name, name, namelen);
    ext2_write_block(new_block, bbuf);
    kfree(bbuf);

    dir_inode.i_block[slot] = new_block;
    dir_inode.i_size += g_block_size;
    dir_inode.i_blocks += g_block_size / BLKDEV_SECTOR_SIZE;
    ext2_write_inode(dir_ino, &dir_inode);
    return 0;
}

/* Remove a directory entry matching child_ino from dir_ino.
 * Merges the freed entry into the previous entry's rec_len. */
static int ext2_dir_remove_entry(uint32_t dir_ino, uint32_t child_ino)
{
    struct ext2_inode dir_inode;
    ext2_read_inode(dir_ino, &dir_inode);

    uint8_t *bbuf = kmalloc(g_block_size);
    if (!bbuf) return -1;

    for (int bi = 0; bi < 12; bi++) {
        uint32_t phys = dir_inode.i_block[bi];
        if (!phys) break;

        ext2_read_block(phys, bbuf);

        uint32_t pos = 0;
        struct ext2_dirent *prev = NULL;
        while (pos < g_block_size) {
            struct ext2_dirent *de = (struct ext2_dirent *)(bbuf + pos);
            if (!de->rec_len) break;
            if (de->inode == child_ino) {
                if (prev) {
                    prev->rec_len += de->rec_len;
                } else {
                    de->inode = 0; /* mark deleted, keep rec_len */
                }
                ext2_write_block(phys, bbuf);
                kfree(bbuf);
                return 0;
            }
            prev = de;
            pos += de->rec_len;
        }
    }

    kfree(bbuf);
    return -1;
}

static struct vfs_node *ext2_make_vfs_node(struct vfs_node *parent,
                                           uint32_t ino,
                                           const char *name,
                                           const struct ext2_inode *inode,
                                           bool is_dir)
{
    struct vfs_node *node = vfs_alloc_node();
    if (!node) return NULL;

    uint32_t nlen = (uint32_t)strlen(name);
    if (nlen >= VFS_NAME_MAX) nlen = VFS_NAME_MAX - 1;
    memcpy(node->name, name, nlen);
    node->name[nlen] = '\0';
    node->inode  = ino;
    node->parent = parent;
    node->fs     = &ext2_ops;

    if (is_dir) {
        node->flags = VFS_DIR;
    } else {
        int slot = ext2_alloc_slot(ino, inode, node);
        if (slot < 0) { vfs_free_node(node); return NULL; }
        node->flags    = VFS_FILE | VFS_CHARDEV;
        node->size     = inode->i_size;
        node->read_op  = g_read_ops[slot];
        node->write_op = g_write_ops[slot];
    }

    vfs_add_child(parent, node);
    return node;
}

static struct vfs_node *ext2_create(struct vfs_node *parent, const char *name)
{
    uint32_t ino = ext2_alloc_inode();
    if (!ino) return NULL;

    uint32_t block = ext2_alloc_block();
    if (!block) { ext2_free_inode(ino); return NULL; }

    /* Zero the data block */
    uint8_t *zbuf = kmalloc(g_block_size);
    if (!zbuf) { ext2_free_block(block); ext2_free_inode(ino); return NULL; }
    memset(zbuf, 0, g_block_size);
    ext2_write_block(block, zbuf);
    kfree(zbuf);

    /* Write inode */
    struct ext2_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode   = EXT2_IMODE_FILE | 0644;
    inode.i_links_count = 1;
    inode.i_block[0]    = block;
    inode.i_blocks      = g_block_size / BLKDEV_SECTOR_SIZE;
    ext2_write_inode(ino, &inode);

    /* Add directory entry (file_type=1 = regular file) */
    if (ext2_dir_add_entry(parent->inode, ino, name, 1) != 0) {
        ext2_free_block(block);
        ext2_free_inode(ino);
        return NULL;
    }

    return ext2_make_vfs_node(parent, ino, name, &inode, false);
}

static struct vfs_node *ext2_mkdir_op(struct vfs_node *parent, const char *name)
{
    uint32_t ino = ext2_alloc_inode();
    if (!ino) return NULL;

    uint32_t block = ext2_alloc_block();
    if (!block) { ext2_free_inode(ino); return NULL; }

    /* Write . and .. entries */
    uint8_t *bbuf = kmalloc(g_block_size);
    if (!bbuf) { ext2_free_block(block); ext2_free_inode(ino); return NULL; }
    memset(bbuf, 0, g_block_size);

    struct ext2_dirent *dot = (struct ext2_dirent *)bbuf;
    dot->inode     = ino;
    dot->rec_len   = 12;
    dot->name_len  = 1;
    dot->file_type = 2;
    dot->name[0]   = '.';

    struct ext2_dirent *dotdot = (struct ext2_dirent *)(bbuf + 12);
    dotdot->inode     = parent->inode;
    dotdot->rec_len   = (uint16_t)(g_block_size - 12);
    dotdot->name_len  = 2;
    dotdot->file_type = 2;
    dotdot->name[0]   = '.';
    dotdot->name[1]   = '.';

    ext2_write_block(block, bbuf);
    kfree(bbuf);

    /* Write inode */
    struct ext2_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode        = EXT2_IMODE_DIR | 0755;
    inode.i_links_count = 2; /* . and parent ref */
    inode.i_size        = g_block_size;
    inode.i_block[0]    = block;
    inode.i_blocks      = g_block_size / BLKDEV_SECTOR_SIZE;
    ext2_write_inode(ino, &inode);

    /* Add directory entry in parent (file_type=2 = directory) */
    if (ext2_dir_add_entry(parent->inode, ino, name, 2) != 0) {
        ext2_free_block(block);
        ext2_free_inode(ino);
        return NULL;
    }

    /* Increment parent's link count for the .. back-reference */
    struct ext2_inode parent_inode;
    ext2_read_inode(parent->inode, &parent_inode);
    parent_inode.i_links_count++;
    ext2_write_inode(parent->inode, &parent_inode);

    return ext2_make_vfs_node(parent, ino, name, &inode, true);
}

static int ext2_unlink(struct vfs_node *parent, struct vfs_node *node)
{
    /* Remove dirent from parent directory */
    if (ext2_dir_remove_entry(parent->inode, node->inode) != 0) return -1;

    /* Read inode, decrement link count, free resources if zero */
    struct ext2_inode inode;
    ext2_read_inode(node->inode, &inode);
    if (inode.i_links_count > 0) inode.i_links_count--;

    if (inode.i_links_count == 0) {
        /* Free all direct blocks */
        for (int bi = 0; bi < 12; bi++) {
            if (inode.i_block[bi]) ext2_free_block(inode.i_block[bi]);
        }
        /* Free single indirect */
        if (inode.i_block[12]) {
            uint32_t *ibuf = kmalloc(g_block_size);
            if (ibuf) {
                ext2_read_block(inode.i_block[12], ibuf);
                uint32_t ptrs = g_block_size / 4;
                for (uint32_t i = 0; i < ptrs; i++)
                    if (ibuf[i]) ext2_free_block(ibuf[i]);
                kfree(ibuf);
            }
            ext2_free_block(inode.i_block[12]);
        }
        inode.i_dtime = 1; /* mark deleted */
        ext2_write_inode(node->inode, &inode);
        ext2_free_inode(node->inode);
    } else {
        ext2_write_inode(node->inode, &inode);
    }

    /* Free the ext2 slot if this node held one */
    for (int i = 0; i < EXT2_FILE_SLOTS; i++) {
        if (g_slots[i].used && g_slots[i].ino == node->inode) {
            g_slots[i].used = false;
            break;
        }
    }

    return 0;
}

static void ext2_free_tree(struct vfs_node *n)
{
    if (!n) return;
    struct vfs_node *c = n->children;
    while (c) {
        struct vfs_node *next = c->sibling;
        ext2_free_tree(c);
        c = next;
    }
    vfs_free_node(n);
}

static int ext2_mount(struct vfs_node *mountpoint, void *device)
{
    g_dev = (struct blkdev *)device;
    if (!g_dev) return -1;

    /* Read superblock: byte offset 1024 = LBA 2, 2 sectors */
    uint8_t sb_buf[2 * BLKDEV_SECTOR_SIZE];
    g_dev->read_sectors(g_dev, 2, 2, sb_buf);
    struct ext2_superblock *sb = (struct ext2_superblock *)(sb_buf + 0);
    /* superblock starts at offset 1024; LBA 2 = byte 1024, so offset within
     * our 2-sector buffer is 0 (we read starting at byte 1024 = LBA 2) */

    if (sb->s_magic != EXT2_MAGIC) {
        printk(MODULE "bad magic: 0x%x\n", sb->s_magic);
        return -1;
    }

    g_block_size      = 1024u << sb->s_log_block_size;
    g_inodes_per_group = sb->s_inodes_per_group;
    g_inode_size      = (sb->s_rev_level >= 1) ? sb->s_inode_size : 128;
    g_bg_count        = (sb->s_blocks_count + sb->s_blocks_per_group - 1)
                        / sb->s_blocks_per_group;

    printk(MODULE "block_size=%ld inode_size=%ld bg_count=%ld\n",
           g_block_size, g_inode_size, g_bg_count);

    /* Read block group descriptor table.
     * Located in block 1 if block_size == 1024, else block 1 (since
     * s_first_data_block==0 for larger blocks). The BGD table always
     * starts at the block immediately after the superblock's block. */
    uint32_t bgdt_block = (g_block_size == 1024) ? 2 : 1;
    uint32_t bgdt_bytes = g_bg_count * sizeof(struct ext2_bgd);
    uint32_t bgdt_blocks = (bgdt_bytes + g_block_size - 1) / g_block_size;

    uint8_t *bgdt_buf = kmalloc(bgdt_blocks * g_block_size);
    if (!bgdt_buf) return -1;
    for (uint32_t i = 0; i < bgdt_blocks; i++)
        ext2_read_block(bgdt_block + i, bgdt_buf + i * g_block_size);

    g_bgds = kmalloc(bgdt_bytes);
    if (!g_bgds) { kfree(bgdt_buf); return -1; }
    memcpy(g_bgds, bgdt_buf, bgdt_bytes);
    kfree(bgdt_buf);

    memset(g_slots, 0, sizeof(g_slots));

    struct vfs_node *root = vfs_alloc_node();
    if (!root) { kfree(g_bgds); return -1; }

    strncpy(root->name, mountpoint->name, VFS_NAME_MAX - 1);
    root->flags  = VFS_DIR;
    root->inode  = EXT2_ROOT_INO;
    root->parent = mountpoint->parent;
    root->fs     = &ext2_ops;

    ext2_walk_dir(root, EXT2_ROOT_INO);

    mountpoint->mounted_root = root;
    return 0;
}

static int ext2_umount(struct vfs_node *mountpoint)
{
    struct vfs_node *root = mountpoint->mounted_root;
    if (!root) return -1;

    ext2_free_tree(root);
    mountpoint->mounted_root = NULL;

    if (g_bgds) { kfree(g_bgds); g_bgds = NULL; }
    memset(g_slots, 0, sizeof(g_slots));
    g_dev = NULL;

    return 0;
}

static struct fs_ops ext2_ops = {
    .mount     = ext2_mount,
    .umount    = ext2_umount,
    .create    = ext2_create,
    .mkdir_op  = ext2_mkdir_op,
    .unlink    = ext2_unlink,
};

void init_ext2(void)
{
    vfs_fs_register("ext2", &ext2_ops);
}
