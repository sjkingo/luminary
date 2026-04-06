#pragma once

#include "cpu/traps.h"
#include "kernel/syscall_numbers.h"

/* Handle a syscall. Called from trap_handler when trapno == SYSCALL_VECTOR. */
void syscall_handler(struct trap_frame *frame);
