# Luminary OS

A small x86 (IA-32) operating system written in C and assembly.

## Requirements

- `i686-elf-gcc` cross-compiler toolchain (`i686-elf-gcc`, `i686-elf-binutils`)
- `python3`
- `qemu-system-i386`
- `xorriso` and `grub2-mkrescue`
- `e2fsprogs` (for ext2 disk image builds)

### macOS (Homebrew)

```bash
brew install i686-elf-gcc i686-elf-binutils qemu python3 xorriso e2fsprogs
```

### Linux (Debian/Ubuntu)

```bash
sudo apt install gcc gcc-multilib make qemu-system-x86 python3 xorriso grub2-common e2fsprogs
```

## Build options

Edit `DEFINES` in `src/Makefile`:

- `-DDEBUG` — debug serial output via `DBGK()`

## Build and run

```bash
make             # build kernel, initrd, ISO, and blank disk image
make ext2fs      # format and populate the disk image with ext2 rootfs
make qemu        # run in QEMU (boots initrd by default; select ext2 from GRUB menu)
make qemu-debug  # same, with GDB stub on port 1234
make clean       # remove all build output
```

Boot targets in GRUB:
- **Luminary (initrd)** — boots from the bundled cpio initrd (default)
- **Luminary (disk)** — mounts ext2 rootfs from the disk image (`root=/dev/hda1`); requires `make ext2fs` first
