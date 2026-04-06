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
    uint8_t  write_refs;    /* number of open write-end fds */
    uint8_t  read_refs;     /* number of open read-end fds */
};

/* Allocate a pipe and return two chardev VFS nodes.
 * read_out  receives the readable end node.
 * write_out receives the writable end node.
 * Returns 0 on success, -1 on allocation failure. */
int pipe_create(struct vfs_node **read_out, struct vfs_node **write_out);

/* Called by sys_close / sys_dup2 when a pipe node fd is closed.
 * Decrements the appropriate ref count; frees the struct pipe when both
 * ref counts reach zero. Safe to call on non-pipe nodes (no-op). */
void pipe_notify_close(struct vfs_node *node);

/* Called by sys_fork for each fd that is a pipe node.
 * Increments write_refs or read_refs to account for the inherited fd. */
void pipe_fork_fd(struct vfs_node *node);

/* Write up to len bytes to a pipe without blocking.
 * Returns the number of bytes written; may be less than len if the buffer
 * is full.  Safe to call from kernel context (no hlt, no scheduling). */
uint32_t pipe_write_nb(struct pipe *p, const char *buf, uint32_t len);

/* Return the struct pipe * for the given pipe node (read or write end).
 * Returns NULL if the node is not a pipe node. */
struct pipe *pipe_get_struct(struct vfs_node *node);

