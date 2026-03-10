#pragma once

#include "cpu/traps.h"

/* Syscall numbers (passed in EAX) */
#define SYS_NOP     0   /* do nothing, return 0 */
#define SYS_WRITE   1   /* write(buf, len) - print to console */
#define SYS_EXIT    2   /* exit() - halt the task */

#define SYS_MAX     2

/* Handle a syscall. Called from trap_handler when trapno == SYSCALL_VECTOR. */
void syscall_handler(struct trap_frame *frame);
