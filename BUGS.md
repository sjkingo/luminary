- Some exceptions are raised with CS set to something other than kernel (0x8),
  which should not happen. Continued exection at this point would cause a GPF.

  see `cpu.c:trap_handler()`
