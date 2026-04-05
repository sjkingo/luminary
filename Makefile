BUILD_DIR   = _build
KERNEL_BIN  = $(BUILD_DIR)/kernel.bin
INITRD_IMG  = $(BUILD_DIR)/initrd.img
ROOTFS_DIR  = $(BUILD_DIR)/rootfs
BIN_DIR     = $(ROOTFS_DIR)/bin
ISO_OUT     = $(BUILD_DIR)/boot.iso
DISK_IMG    = $(BUILD_DIR)/disk.img
E2FS_PREFIX = $(shell brew --prefix e2fsprogs 2>/dev/null)
MKE2FS      = $(E2FS_PREFIX)/sbin/mke2fs

EXT2FS_PART_LBA    = 2048
EXT2FS_TOTAL_SECTS = 204800
EXT2FS_PART_SECTS  = $(shell echo $$(($(EXT2FS_TOTAL_SECTS) - $(EXT2FS_PART_LBA))))
EXT2FS_OFFSET      = $(shell echo $$(($(EXT2FS_PART_LBA) * 512)))
EXT2FS_BLOCKS      = $(shell echo $$(($(EXT2FS_PART_SECTS) / 2)))

EMULATOR    = qemu-system-i386
QEMU_ARGS   = -m 512 -net nic,model=rtl8139 -net user -serial file:/tmp/luminary.log \
		-cdrom $(ISO_OUT) -drive file=$(DISK_IMG),format=raw,index=0,media=disk -boot d

.PHONY: all
all: bootiso ext2fs

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

.PHONY: kernel
kernel: | $(BUILD_DIR)
	$(MAKE) -C src BUILD_DIR=$(abspath $(BUILD_DIR))

.PHONY: userland
userland: | $(BIN_DIR)
	$(MAKE) -C userland BIN_DIR=$(abspath $(BIN_DIR))

.PHONY: initrd
initrd: userland | $(BUILD_DIR)
	cp -r rootfs/. $(ROOTFS_DIR)/
	cd $(ROOTFS_DIR) && find . | sort | cpio -o --format=newc > $(abspath $(INITRD_IMG))
	@echo "initrd: built $$(wc -c < $(INITRD_IMG)) bytes"

.PHONY: bootiso
bootiso: kernel initrd
	$(MAKE) -C grub2 \
		KERNEL_BIN=$(abspath $(KERNEL_BIN)) \
		INITRD_IMG=$(abspath $(INITRD_IMG)) \
		ISO_OUT=$(abspath $(ISO_OUT))


$(DISK_IMG): | $(BIN_DIR) $(BUILD_DIR)
	$(MAKE) -C userland BIN_DIR=$(abspath $(BIN_DIR))
	cp -r rootfs/. $(ROOTFS_DIR)/
	dd if=/dev/zero of=$(DISK_IMG) bs=512 count=$(EXT2FS_TOTAL_SECTS)
	python3 tools/mkdisk.py $(DISK_IMG)
	$(MKE2FS) -t ext2 -b 1024 -E offset=$(EXT2FS_OFFSET) -d $(ROOTFS_DIR) -F $(DISK_IMG) $(EXT2FS_BLOCKS)
	@echo "ext2fs: formatted and populated $(DISK_IMG) from $(ROOTFS_DIR)"

.PHONY: ext2fs
ext2fs: $(DISK_IMG)

.PHONY: bootiso-ext2
bootiso-ext2: kernel ext2fs

.PHONY: qemu
qemu: all
	$(EMULATOR) $(QEMU_ARGS)
	cat /tmp/luminary.log

.PHONY: qemu-debug
qemu-debug: all
	$(EMULATOR) -s -S $(QEMU_ARGS)
	cat /tmp/luminary.log

.PHONY: gdb
gdb:
	gdb -x tools/gdbinitrc

.PHONY: clean
clean:
	$(MAKE) -C src clean
	$(MAKE) -C userland clean
	rm -rf $(BUILD_DIR)
