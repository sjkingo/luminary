/* env_dev.c — /dev/env chardev for per-task environment variable access.
 *
 * Programs open /dev/env and issue ioctl() with ENV_* request codes.
 * All operations act on the calling task's environ table (running_task).
 *
 * The environ table is stored inline in struct task as
 * environ[TASK_ENVIRON_MAX][TASK_ENVIRON_LEN] (32 × 128 bytes = 4KB).
 * Fork copies it automatically via *child = *running_task.
 * SYS_EXECVE replaces it with the supplied envp (or preserves it when envp=NULL).
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "kernel/kernel.h"
#include "kernel/vmm.h"
#include "kernel/vfs.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/env_dev.h"

static inline bool user_access_ok(const void *ptr, uint32_t len)
{
    uint32_t addr = (uint32_t)ptr;
    if (addr < USER_SPACE_START) return false;
    if (addr >= USER_SPACE_END)  return false;
    if (len > 1 && len > USER_SPACE_END - addr) return false;
    return true;
}

/* Find the index of name in the environ table, or -1 if not found. */
static int environ_find(struct task *t, const char *name)
{
    unsigned int nlen = (unsigned int)strlen(name);
    for (int i = 0; i < t->environ_count; i++) {
        const char *e = t->environ[i];
        unsigned int j = 0;
        while (e[j] && e[j] != '=' && j < nlen) j++;
        if (j == nlen && e[j] == '=')
            return i;
    }
    return -1;
}

static int32_t env_control_op(struct vfs_node *node, uint32_t request, void *arg)
{
    (void)node;

    struct env_op *op = (struct env_op *)arg;
    if (!user_access_ok(op, sizeof(struct env_op))) return -1;

    struct task *t = running_task;

    switch (request) {

    case ENV_GET: {
        int idx = environ_find(t, op->name);
        if (idx < 0) return -1;
        const char *e = t->environ[idx];
        /* skip past '=' */
        while (*e && *e != '=') e++;
        if (*e == '=') e++;
        strncpy(op->val, e, sizeof(op->val) - 1);
        op->val[sizeof(op->val) - 1] = '\0';
        return 0;
    }

    case ENV_SET: {
        /* Reject name containing '=' */
        for (unsigned int i = 0; op->name[i]; i++)
            if (op->name[i] == '=') return -1;

        int idx = environ_find(t, op->name);
        if (idx >= 0) {
            if (!op->overwrite) return 0;
        } else {
            if (t->environ_count >= TASK_ENVIRON_MAX) return -1;
            idx = t->environ_count++;
        }
        char *dst = t->environ[idx];
        unsigned int n = 0;
        for (unsigned int i = 0; op->name[i] && n < TASK_ENVIRON_LEN - 2; i++)
            dst[n++] = op->name[i];
        dst[n++] = '=';
        for (unsigned int i = 0; op->val[i] && n < TASK_ENVIRON_LEN - 1; i++)
            dst[n++] = op->val[i];
        dst[n] = '\0';
        return 0;
    }

    case ENV_UNSET: {
        int idx = environ_find(t, op->name);
        if (idx < 0) return 0;
        /* Shift entries down to fill the gap */
        for (int i = idx; i < t->environ_count - 1; i++)
            memcpy(t->environ[i], t->environ[i + 1], TASK_ENVIRON_LEN);
        t->environ_count--;
        return 0;
    }

    case ENV_GET_IDX: {
        int idx = op->index;
        if (idx < 0 || idx >= t->environ_count) return -1;
        strncpy(op->val, t->environ[idx], sizeof(op->val) - 1);
        op->val[sizeof(op->val) - 1] = '\0';
        return 0;
    }

    default:
        return -1;
    }
}

void init_dev_env(void)
{
    if (!vfs_register_dev("env", 112, NULL, NULL, env_control_op))
        panic("init_dev_env: failed to register /dev/env");
    printk("devfs: /dev/env registered\n");
}
