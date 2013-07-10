KERNEL_BIN=kernel.bin
EMULATOR=qemu-system-i386

OBJS=	boot.o \
	cpu.o \
	kernel.o \
	printk.o \
	tables.o \
	vga.o

CC=gcc
AS=as

CFLAGS=$(ARCH) -Wall -Wextra -ffreestanding -std=gnu99
ASFLAGS=--32
LDFLAGS=-T linker.ld -nostdlib -lgcc
ARCH=-m32

.PHONY: all
all: $(KERNEL_BIN)

$(KERNEL_BIN): linker.ld $(OBJS)
	$(CC) $(ARCH) $(LDFLAGS) -o $@ $(OBJS)

.PHONY: qemu
qemu: $(KERNEL_BIN)
	$(EMULATOR) -kernel $(KERNEL_BIN)

.PHONY: qemucd
qemucd: $(KERNEL_BIN) bootiso
	$(EMULATOR) -cdrom ../grub2/boot.iso

.PHONY: clean
clean:
	rm -f *.o $(KERNEL_BIN) core.* *.d
	make -C ../grub2 clean

.PHONY: bootiso
bootiso:
	make -C ../grub2 boot.iso

# pull in existing deps for all our objects
-include $(OBJS:.o=.d)

%.o: %.c
	$(CC) $(CFLAGS) -c $*.c -o $*.o
	$(CC) $(CFLAGS) -MM $*.c > $*.d
