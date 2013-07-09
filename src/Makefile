KERNEL_BIN=kernel.bin
EMULATOR=qemu-system-i386

CC=gcc
AS=as

CFLAGS=$(ARCH) -Wall -Wextra -ffreestanding -std=gnu99
ASFLAGS=--32
LDFLAGS=-T linker.ld -nostdlib -lgcc
ARCH=-m32

.PHONY: all
all: $(KERNEL_BIN)

$(KERNEL_BIN): boot.o kernel.o linker.ld
	$(CC) $(ARCH) $(LDFLAGS) -o $@ $^

.PHONY: qemu
qemu: $(KERNEL_BIN)
	$(EMULATOR) -kernel $(KERNEL_BIN)

.PHONY: qemucd
qemucd: $(KERNEL_BIN) bootiso
	$(EMULATOR) -cdrom ../grub2/boot.iso

.PHONY: clean
clean:
	rm -f *.o $(KERNEL_BIN) core.*
	make -C ../grub2 clean

.PHONY: bootiso
bootiso:
	make -C ../grub2 boot.iso
