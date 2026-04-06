/* sys_dev.c — /dev/sys chardev for system control and information.
 *
 * Replaces SYS_HALT, SYS_REBOOT, SYS_UPTIME, SYS_PS, and SYS_MOUNT syscalls.
 * Programs open /dev/sys and issue ioctl() calls with SYS_CTL_* request codes.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "kernel/kernel.h"
#include "kernel/vmm.h"
#include "kernel/vfs.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/sys_dev.h"
#include "drivers/keyboard.h"

static inline bool user_access_ok(const void *ptr, uint32_t len)
{
    uint32_t addr = (uint32_t)ptr;
    if (addr < USER_SPACE_START) return false;
    if (addr >= USER_SPACE_END)  return false;
    if (len > 1 && len > USER_SPACE_END - addr) return false;
    return true;
}

/* ── control op ──────────────────────────────────────────────────────────── */

static int32_t sys_control_op(struct vfs_node *node, uint32_t request, void *arg)
{
    (void)node;
    switch (request) {

    case SYS_CTL_HALT:
        printk("system halted via /dev/sys\n");
        extern void cpu_halt(void);
        cpu_halt();
        return 0; /* unreachable */

    case SYS_CTL_REBOOT:
        printk("system reboot via /dev/sys\n");
        extern void cpu_reboot(void);
        cpu_reboot();
        return 0; /* unreachable */

    case SYS_CTL_UPTIME: {
        uint32_t *out = (uint32_t *)arg;
        if (!user_access_ok(out, sizeof(uint32_t))) return -1;
        *out = (uint32_t)timekeeper.uptime_ms;
        return 0;
    }

    case SYS_CTL_PS: {
        struct sys_ctl_ps *r = (struct sys_ctl_ps *)arg;
        if (!user_access_ok(r, sizeof(struct sys_ctl_ps))) return -1;
        if (!r->buf || r->len == 0) return -1;
        if (!user_access_ok(r->buf, r->len)) return -1;

        char *buf = r->buf;
        uint32_t buflen = r->len;
        uint32_t pos = 0;
        char tmp[128];

        const char *hdr = "PID\tPPID\tPRIO\tTIME\tCPU\tSTATE\tCMD\n";
        uint32_t i = 0;
        while (hdr[i] && pos < buflen - 1) buf[pos++] = hdr[i++];

        struct task *tasks[64];
        int ntasks = 0;
        struct task *t = sched_queue;
        while (t && ntasks < 64) {
            tasks[ntasks++] = t;
            t = t->next;
        }

        for (int j = 1; j < ntasks; j++) {
            struct task *key = tasks[j];
            int k = j - 1;
            while (k >= 0 && tasks[k]->pid > key->pid) {
                tasks[k + 1] = tasks[k];
                k--;
            }
            tasks[k + 1] = key;
        }

        for (int j = 0; j < ntasks && pos < buflen - 1; j++) {
            t = tasks[j];
            uint32_t age_s = (timekeeper.uptime_ms - t->created) / 1000;
            uint32_t cpu_pct = t->cpu_pct;
            const char *state;
            if (t->pid == PID_IDLE)                  state = "I";
            else if (t == running_task)              state = "R";
            else if (t->blocking)                    state = "B";
            else if (t->wait_pid != -1)              state = "W";
            else if (t->prio_d == SCHED_LEVEL_SUSP)  state = "S";
            else                                     state = "D";
            const char *cmd = t->cmdline[0] ? t->cmdline : t->name;
            int n = sprintf(tmp, "%d\t%d\t%d\t%lu\t%lu\t%s\t%s\n",
                            t->pid, t->ppid, t->prio_s,
                            (unsigned long)age_s, (unsigned long)cpu_pct,
                            state, cmd);
            for (int k = 0; k < n && pos < buflen - 1; k++)
                buf[pos++] = tmp[k];
        }
        buf[pos] = '\0';
        r->written = pos;
        return 0;
    }

    case SYS_CTL_MOUNTS: {
        struct sys_ctl_mounts *r = (struct sys_ctl_mounts *)arg;
        if (!user_access_ok(r, sizeof(struct sys_ctl_mounts))) return -1;
        if (!r->buf || r->len == 0) return -1;
        if (!user_access_ok(r->buf, r->len)) return -1;

        struct vfs_mount_info mounts[VFS_MOUNT_MAX];
        int nmounts = vfs_get_mounts(mounts, VFS_MOUNT_MAX);

        uint32_t pos = 0;
        char *buf = r->buf;
        uint32_t buflen = r->len - 1;

        const char *hdr = "MOUNT\t\tTYPE    OPTIONS\n";
        while (*hdr && pos < buflen) buf[pos++] = *hdr++;

        for (int i = 0; i < nmounts && pos < buflen; i++) {
            const char *opts = mounts[i].readonly ? "ro" : "rw";
            char line[64];
            int n = sprintf(line, "%-8s\t%-7s %s\n",
                            mounts[i].path, mounts[i].fstype, opts);
            for (int j = 0; j < n && pos < buflen; j++)
                buf[pos++] = line[j];
        }

        buf[pos] = '\0';
        r->written = pos;
        return 0;
    }

    case SYS_CTL_GUI_ACTIVE: {
        uint32_t *out = (uint32_t *)arg;
        if (!user_access_ok(out, sizeof(uint32_t))) return -1;
        *out = kbd_is_owned() ? 1u : 0u;
        return 0;
    }

    default:
        return -1;
    }
}

/* ── init ────────────────────────────────────────────────────────────────── */

void init_dev_sys(void)
{
    if (!vfs_register_dev("sys", 111, NULL, NULL, sys_control_op))
        panic("init_dev_sys: failed to register /dev/sys");
    printk("devfs: /dev/sys registered\n");
}
