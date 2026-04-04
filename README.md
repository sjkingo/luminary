# Luminary OS

A small x86 (IA-32) operating system written in C and assembly.

## Requirements

- `i686-elf-gcc` cross-compiler toolchain (`i686-elf-gcc`, `i686-elf-binutils`)
- `python3`
- `qemu-system-i386`
- `xorriso` and `grub2-mkrescue`

### macOS (Homebrew)

```bash
brew install i686-elf-gcc i686-elf-binutils qemu python3 xorriso
```

### Linux (Debian/Ubuntu)

```bash
sudo apt install gcc gcc-multilib make qemu-system-x86 python3 xorriso grub2-common
```

## Build options

Edit `DEFINES` in `src/Makefile`:

- `-DDEBUG` — debug serial output via `DBGK()`

## Build and run

```bash
make        # build everything
make qemucd # build and run in QEMU via GRUB2 ISO
make clean  # remove all build output
```
