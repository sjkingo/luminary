#pragma once

#include "kernel/vfs.h"

struct task; /* forward declaration — avoid circular include with task.h */

/* Device nodes for the three standard streams. NULL until init_devfs() runs. */
extern struct vfs_node *dev_stdin;
extern struct vfs_node *dev_stdout;
extern struct vfs_node *dev_stderr;

/* Create /dev directory and populate it with stdin/stdout/stderr chardevs.
 * Must be called after initrd_init() (VFS root must exist). */
void init_devfs(void);

/* Pre-open fds 0/1/2 on a newly created task.
 * No-op if init_devfs() has not yet run. */
void task_open_std_fds(struct task *t);
