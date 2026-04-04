#pragma once

#include <stdint.h>
#include "kernel/vfs.h"

struct task; /* forward declaration */

/* Size of the pipe ring buffer. Must be a power of two. */
#define PIPE_BUF_SIZE 4096

/* Shared state for one pipe (one ring buffer, two endpoints). */
struct pipe {
    char     buf[PIPE_BUF_SIZE];
    uint32_t head;          /* write index */
    uint32_t tail;          /* read index */
    int      write_closed;  /* set when write end is closed */
    int      read_closed;   /* set when read end is closed */
};

/* Allocate a pipe and return two chardev VFS nodes.
 * read_out  receives the readable end node.
 * write_out receives the writable end node.
 * Returns 0 on success, -1 on allocation failure. */
int pipe_create(struct vfs_node **read_out, struct vfs_node **write_out);

/* Called by sys_close when a pipe node is closed.
 * Updates write_closed/read_closed and frees the struct pipe when both
 * ends are closed. Safe to call on non-pipe nodes (no-op). */
void pipe_notify_close(struct vfs_node *node);

