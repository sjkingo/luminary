KERNEL_BIN=kernel.bin
EMULATOR=qemu-system-i386
QEMU_ARGS=-net nic,model=rtl8139 -net user
DEFINES=-DDEBUG -DTURTLE -DUSE_SERIAL

# Order must be: 1. boot, 2. kernel, ...
include boot/Makefile.mk
include kernel/Makefile.mk
include drivers/Makefile.mk
include cpu/Makefile.mk
include pci/Makefile.mk

include lib/Makefile.mk

CC=gcc
AS=$(CC)

ARCH=-m32
CFLAGS=$(ARCH) -Wall -Wextra -ffreestanding -std=gnu99 -g -I . $(DEFINES)
ASFLAGS=$(ARCH)
LDFLAGS=-T linker.ld -nostdlib -lgcc

.PHONY: all
all: $(KERNEL_BIN)

$(KERNEL_BIN): linker.ld $(KERNEL_OBJS)
	$(CC) $(ARCH) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

.PHONY: qemu
qemu: $(KERNEL_BIN)
	$(EMULATOR) -kernel $(KERNEL_BIN) $(QEMU_ARGS)

.PHONY: qemucd
qemucd: $(KERNEL_BIN) bootiso
	$(EMULATOR) -cdrom ../grub2/boot.iso $(QEMU_ARGS)

.PHONY: console
console: $(KERNEL_BIN)
	$(EMULATOR) -kernel $(KERNEL_BIN) -nographic $(QEMU_ARGS)

.PHONY: debug
debug: $(KERNEL_BIN)
	$(EMULATOR) -kernel $(KERNEL_BIN) -s -S $(QEMU_ARGS)

.PHONY: clean
clean:
	rm -f $(KERNEL_OBJS) $(KERNEL_OBJS:.o=.d) $(KERNEL_BIN) core.* cpu/vectors.S version.h
	make -C ../grub2 clean

.PHONY: bootiso
bootiso:
	make -C ../grub2 boot.iso

# pull in existing deps for all our objects
-include $(KERNEL_OBJS:.o=.d)

# version.h is auto-generated
version.h: ../README.md
	../scripts/make_version_h.py > version.h

kernel/kernel.o: version.h kernel/kernel.c

cpu/vectors.S: cpu/mkvectors.py
	python cpu/mkvectors.py > $@

%.o: %.c
	$(CC) $(CFLAGS) -c $*.c -o $*.o
	$(CC) $(CFLAGS) -MM $*.c > $*.d
