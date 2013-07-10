Luminary, a small x86 RTOS operating system
===========================================

To build, enter into the `src` subdirectory and type `make`. You will need to
have a working `gcc` and GNU `as` compiler that is capable of producing 32-bit
executables.

After building, you may run the kernel in QEMU in one of two ways:

`make qemu` to have QEMU load the kernel image directly, or

`make qemucd` to build a bootable ISO image with Grub 2 and boot that way

Some architecture notes are in `src/NOTES.md`.
