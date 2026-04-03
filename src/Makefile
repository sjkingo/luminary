# Set configuration variables for the build
KERNEL_BIN=	kernel.bin
INITRD_IMG=	../initrd.img
EMULATOR=	qemu-system-i386
QEMU_ARGS=	-net nic,model=rtl8139 -net user -serial file:/tmp/luminary.log #-nographic
#DEFINES=	-DDEBUG -DTURTLE -DUSE_SERIAL
DEFINES=	-DDEBUG -DUSE_SERIAL

CROSS=		i686-elf-
CC=		$(CROSS)gcc
AS=		$(CC)
ARCH=		-m32
CFLAGS=		$(ARCH) -Wall -Wextra -Wno-unused-variable -Wno-unused-function -ffreestanding -std=gnu99 -g -I . -isystem include $(DEFINES)
ASFLAGS=	$(ARCH)
LDFLAGS=	-T linker.ld -nostdlib -lgcc

# Include our submodules that contain all the object files
# Order must be: 1. boot, 2. kernel, ...
include boot/Makefile.mk
include kernel/Makefile.mk
include cpu/Makefile.mk
include drivers/Makefile.mk
include lib/Makefile.mk
include pci/Makefile.mk

.PHONY: all
all: $(KERNEL_BIN) initrd

$(KERNEL_BIN): linker.ld $(KERNEL_OBJS)
	$(CC) $(ARCH) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

.PHONY: userspace
userspace:
	make -C ../userland

# Build the cpio initrd from ../rootfs/
# Copy freshly-built ELF binaries into rootfs/bin, then pack with cpio.
.PHONY: initrd
initrd: userspace
	cp ../userland/init.elf ../rootfs/bin/init
	cp ../userland/sh.elf   ../rootfs/bin/sh
	cp ../userland/gui.elf  ../rootfs/bin/gui
	cd ../rootfs && find . | sort | cpio -o --format=newc > ../initrd.img
	@echo "initrd: built $$(wc -c < ../initrd.img) bytes"

.PHONY: qemu
qemu: $(KERNEL_BIN) initrd
	$(EMULATOR) -kernel $(KERNEL_BIN) -initrd "$(INITRD_IMG)" $(QEMU_ARGS)

.PHONY: qemucd
qemucd: $(KERNEL_BIN) initrd bootiso
	$(EMULATOR) -cdrom ../grub2/boot.iso $(QEMU_ARGS)
	cat /tmp/luminary.log

.PHONY: console
console: $(KERNEL_BIN) initrd
	$(EMULATOR) -kernel $(KERNEL_BIN) -initrd "$(INITRD_IMG)" -nographic $(QEMU_ARGS) &
	sleep 2
	pkill qemu-system-*

.PHONY: debug
debug: $(KERNEL_BIN) initrd
	$(EMULATOR) -kernel $(KERNEL_BIN) -initrd "$(INITRD_IMG)" -s -S $(QEMU_ARGS)

.PHONY: qemudebug
qemudebug: $(KERNEL_BIN) initrd
	$(EMULATOR) -kernel $(KERNEL_BIN) -initrd "$(INITRD_IMG)" $(QEMU_ARGS) -serial file:/tmp/luminary-serial.log

.PHONY: gdb
gdb:
	gdb -x ../tools/gdbinitrc

.PHONY: clean
clean:
	rm -f $(KERNEL_OBJS) $(KERNEL_OBJS:.o=.d) $(KERNEL_BIN) core.* cpu/vectors.S version.h
	rm -f $(INITRD_IMG)
	make -C ../grub2 clean
	make -C ../userland clean

.PHONY: bootiso
bootiso:
	make -C ../grub2 boot.iso

# pull in existing deps for all our objects
-include $(KERNEL_OBJS:.o=.d)

# this is a PHONY so it is regenerated every time
.PHONY: version_header
version_header:
	../tools/make_version_h.sh > version.h

kernel/kernel.o: version_header kernel/kernel.c

cpu/vectors.S: cpu/mkvectors.py
	python3 cpu/mkvectors.py > $@

cpu/traps.o: kernel/task.h

%.o: %.c
	$(CC) $(CFLAGS) -c $*.c -o $*.o
	$(CC) $(CFLAGS) -MM $*.c > $*.d
