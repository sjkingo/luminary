/* sys_dev.h — /dev/sys ioctl request codes and argument structs.
 *
 * Userland mirrors these in userland/sys_dev.h.
 */
#pragma once

#include <stdint.h>

/* ── request codes ───────────────────────────────────────────────────────── */
#define SYS_CTL_HALT       1   /* no arg — halt the machine */
#define SYS_CTL_REBOOT     2   /* no arg — reboot the machine */
#define SYS_CTL_UPTIME     3   /* arg: uint32_t * — filled with uptime in ms */
#define SYS_CTL_PS         4   /* arg: struct sys_ctl_ps * */
#define SYS_CTL_MOUNTS     5   /* no arg — print mount table to kernel console */
#define SYS_CTL_GUI_ACTIVE 6   /* arg: uint32_t * — set to 1 if GUI has keyboard */

/* ── argument structs ────────────────────────────────────────────────────── */

struct sys_ctl_ps {
    char    *buf;
    uint32_t len;
    uint32_t written;
};

struct sys_ctl_mounts {
    char    *buf;
    uint32_t len;
    uint32_t written;
};

void init_dev_sys(void);
