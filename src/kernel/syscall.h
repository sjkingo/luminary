#pragma once

#include "cpu/traps.h"

/* Syscall numbers (passed in EAX) */
#define SYS_NOP     0   /* do nothing, return 0 */
#define SYS_WRITE   1   /* write(buf, len) - print to console */
#define SYS_EXIT    2   /* exit() - halt the task */
#define SYS_READ    3   /* read(buf, len) - read from keyboard */
#define SYS_UPTIME  4   /* uptime() - return uptime in ms */
#define SYS_GETPID  5   /* getpid() - return current task PID */
#define SYS_HALT    6   /* halt() - shut down the machine */
#define SYS_PS          7   /* ps() - print task list to console */

/* GUI / window manager syscalls
 * Calling convention: EAX=syscall, EBX=arg0, ECX=arg1, EDX=arg2
 * Additional args read from user stack at [uesp+4], [uesp+8], ...
 */
#define SYS_WIN_CREATE      8   /* (x, y, w, h) + title ptr at uesp+4 -> id */
#define SYS_WIN_DESTROY     9   /* (id) */
#define SYS_WIN_FILL_RECT   10  /* (id, x, y) + w,h,color at uesp+4..12 */
#define SYS_WIN_DRAW_TEXT   11  /* (id, x, y) + str,fgcolor,bgcolor at uesp+4..12 */
#define SYS_WIN_FLIP        12  /* (id) */
#define SYS_WIN_POLL_EVENT  13  /* (id, event_buf_ptr) -> 0 or 1 */
#define SYS_MOUSE_GET       14  /* (mouse_state_buf_ptr) -> buttons<<16|1 or 0 */
#define SYS_WIN_DRAW_RECT   15  /* (id, x, y) + w,h,color at uesp+4..12 */
#define SYS_EXEC            16  /* exec(module_index) - spawn ELF module as task -> pid or -1 */
#define SYS_KILL            17  /* kill(pid) -> 0 on success, -1 if not found */
#define SYS_YIELD           18  /* yield() - hlt and let scheduler run */

#define SYS_MAX     18

/* Handle a syscall. Called from trap_handler when trapno == SYSCALL_VECTOR. */
void syscall_handler(struct trap_frame *frame);
