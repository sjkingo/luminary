KERNEL_BIN=kernel.bin
EMULATOR=qemu-system-i386
QEMU_ARGS=-net nic,model=rtl8139 -net user
DEFINES=-DDEBUG -DTURTLE -DUSE_SERIAL

OBJS=	boot/boot.o \
	cpu/cpu.o \
	pci/register.o \
	drivers/rtl8139.o \
	kernel/kernel.o \
	pci/pci.o \
	cpu/pic.o \
	kernel/printk.o \
	sched.o \
	serial.o \
	string.o \
	cpu/tables.o \
	task.o \
	cpu/traps.o \
	cpu/vectors.o \
	vga.o

CC=gcc
AS=$(CC)

ARCH=-m32
CFLAGS=$(ARCH) -Wall -Wextra -ffreestanding -std=gnu99 -g -I . $(DEFINES)
ASFLAGS=$(ARCH)
LDFLAGS=-T linker.ld -nostdlib -lgcc

.PHONY: all
all: pre_build $(KERNEL_BIN)

.PHONY: pre_build
pre_build:
	../scripts/make_version_h.py > version.h

$(KERNEL_BIN): linker.ld $(OBJS)
	$(CC) $(ARCH) $(LDFLAGS) -o $@ $(OBJS)

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
	rm -f $(OBJS) $(OBJS:.o=.d) $(KERNEL_BIN) core.* cpu/vectors.S version.h
	make -C ../grub2 clean

.PHONY: bootiso
bootiso:
	make -C ../grub2 boot.iso

# pull in existing deps for all our objects
-include $(OBJS:.o=.d)

cpu/vectors.S: cpu/mkvectors.py
	python cpu/mkvectors.py > $@

kernel.o: pre_build kernel.c

%.o: %.c
	$(CC) $(CFLAGS) -c $*.c -o $*.o
	$(CC) $(CFLAGS) -MM $*.c > $*.d
