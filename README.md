# Luminary, a small x86 real-time operating system

## Features

* Small codebase
* Hard priority-based [preemptive scheduler](https://github.com/sjkingo/luminary/blob/master/src/sched.c#L1-L82)
* Serial console support

Some architecture notes and *gotchas* are located in `NOTES.md`.

## Build requirements

* `gcc` and GNU `as` compilers that are capable of producing 32-bit executables
* `qemu`
* `glibc-devel.i686`
* `xorriso` for making a boot ISO (optional)

Tested as working under `gcc` 5.1.1 and GNU `as` 2.25-5

## Build configuration

You may configure the build by editing the `$DEFINES` variable at the top of [`src/Makefile`](https://github.com/sjkingo/luminary/blob/master/src/Makefile#L3).

Available options are:

* `-DDEBUG`: produce debugging output to the console. You probably want this with `-DTURTLE`
* `-DTURTLE`: scale the scheduler down to 1 task per second
* `-DUSE_SERIAL`: enable the serial subsystem, which writes console output to COM1. This may be used with `qemu -nographic`.

## How to build

Building the kernel is as simple as running the included `Makefile`:

```bash
$ make -C src
```

You will need to have a working `gcc` and GNU `as` compiler that is capable of
producing 32-bit executables.

## Running

After building, you may run the kernel in QEMU in one of two ways:

To have QEMU load the kernel image directly (fastest):

```bash
$ make qemu
```

Or to build a bootable ISO image with Grub2 Multiboot and boot that way (requires `xorriso`):

```bash
$ make qemucd
```
