- Some exceptions are raised with CS set to something other than kernel (0x8),
  which should not happen. Continued exection at this point would cause a GPF.

  see `cpu.c:trap_handler()`

- (following thoughts from above..) What are the correct segment selectors for
  kernel code and data segments? We seem to use or come across 0x8, 0x10 (16h),
  0x12 (18h). Confirm that we are using the correct ones in places such as
  GDT/IDT setup, and trap handling.
