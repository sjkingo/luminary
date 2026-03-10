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
#define SYS_PS      7   /* ps() - print task list to console */

#define SYS_MAX     7

/* Handle a syscall. Called from trap_handler when trapno == SYSCALL_VECTOR. */
void syscall_handler(struct trap_frame *frame);
