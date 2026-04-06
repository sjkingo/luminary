/* sys_dev.h — userland /dev/sys ioctl API. */
#pragma once

#include "syscall.h"

/* ── request codes ───────────────────────────────────────────────────────── */
#define SYS_CTL_HALT       1
#define SYS_CTL_REBOOT     2
#define SYS_CTL_UPTIME     3
#define SYS_CTL_PS         4
#define SYS_CTL_MOUNTS     5
#define SYS_CTL_GUI_ACTIVE 6

/* ── argument structs ────────────────────────────────────────────────────── */

struct sys_ctl_ps {
    char        *buf;
    unsigned int len;
    unsigned int written;
};

struct sys_ctl_mounts {
    char        *buf;
    unsigned int len;
    unsigned int written;
};

/* ── wrappers ─────────────────────────────────────────────────────────────── */

static inline void sys_halt(int sfd)
{
    ioctl(sfd, SYS_CTL_HALT, (void *)0);
}

static inline void sys_reboot(int sfd)
{
    ioctl(sfd, SYS_CTL_REBOOT, (void *)0);
}

static inline unsigned int sys_uptime(int sfd)
{
    unsigned int ms = 0;
    ioctl(sfd, SYS_CTL_UPTIME, &ms);
    return ms;
}

static inline int sys_ps(int sfd, char *buf, unsigned int len)
{
    struct sys_ctl_ps req = { buf, len, 0 };
    if (ioctl(sfd, SYS_CTL_PS, &req) < 0) return -1;
    return (int)req.written;
}

static inline int sys_mounts(int sfd, char *buf, unsigned int len)
{
    struct sys_ctl_mounts req = { buf, len, 0 };
    if (ioctl(sfd, SYS_CTL_MOUNTS, &req) < 0) return -1;
    return (int)req.written;
}
