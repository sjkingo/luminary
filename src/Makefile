# Kernel-only Makefile.  Builds kernel.bin into BUILD_DIR (set by root Makefile).

BUILD_DIR  ?= ../_build
KERNEL_BIN  = $(BUILD_DIR)/kernel.bin

CROSS=		i686-elf-
CC=		$(CROSS)gcc
AS=		$(CC)
NM=		$(CROSS)nm
ADDR2LINE=	$(CROSS)addr2line

ARCH=		-m32
DEFINES=	-DDEBUG
CFLAGS=		$(ARCH) -Wall -Wextra -Wno-unused-variable -Wno-unused-function -ffreestanding -std=gnu99 -g -I . -isystem include $(DEFINES)
ASFLAGS=	$(ARCH)
LDFLAGS=	-T linker.ld -nostdlib -lgcc

# Include submodule object lists.  Order must be: 1. boot, 2. kernel, ...
include boot/Makefile.mk
include kernel/Makefile.mk
include cpu/Makefile.mk
include drivers/Makefile.mk
include lib/Makefile.mk
include pci/Makefile.mk
include fs/Makefile.mk

.PHONY: all
all: $(KERNEL_BIN)

# Two-pass build to embed symbol names.
# Pass 1: link without symtab_gen.o to get stable addresses.
# Generate symtab_gen.c, compile it, then final link.
KERNEL_OBJS_NO_SYMS = $(filter-out kernel/symtab_gen.o kernel/symtab.o, $(KERNEL_OBJS))

kernel/symtab_gen.c: $(KERNEL_OBJS_NO_SYMS) linker.ld
	$(CC) $(ARCH) $(LDFLAGS) -Wl,--unresolved-symbols=ignore-all -o kernel.bin.pass1 $(KERNEL_OBJS_NO_SYMS) 2>/dev/null || true
	../tools/gen_symtab.sh $(NM) $(ADDR2LINE) kernel.bin.pass1 kernel/symtab_gen.c
	rm -f kernel.bin.pass1

kernel/symtab_gen.o: kernel/symtab_gen.c
	$(CC) $(CFLAGS) -c kernel/symtab_gen.c -o kernel/symtab_gen.o

$(KERNEL_BIN): linker.ld $(KERNEL_OBJS) | $(BUILD_DIR)
	$(CC) $(ARCH) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

.PHONY: clean
clean:
	rm -f $(KERNEL_OBJS) $(KERNEL_OBJS:.o=.d) core.* cpu/vectors.S
	rm -f kernel/symtab_gen.c kernel/symtab_gen.o kernel/symtab_gen.d kernel.bin.pass1

-include $(KERNEL_OBJS:.o=.d)

kernel/kernel.o: kernel/kernel.c

cpu/vectors.S: cpu/mkvectors.py
	python3 cpu/mkvectors.py > $@

cpu/traps.o: kernel/task.h

%.o: %.c
	$(CC) $(CFLAGS) -c $*.c -o $*.o
	$(CC) $(CFLAGS) -MM $*.c > $*.d
