BUILD_DIR   = _build
KERNEL_BIN  = $(BUILD_DIR)/kernel.bin
INITRD_IMG  = $(BUILD_DIR)/initrd.img
ROOTFS_DIR  = $(BUILD_DIR)/rootfs
BIN_DIR     = $(ROOTFS_DIR)/bin
ISO_OUT     = $(BUILD_DIR)/boot.iso
DISK_IMG    = $(BUILD_DIR)/disk.img

EMULATOR    = qemu-system-i386
QEMU_ARGS   = -m 512 -net nic,model=rtl8139 -net user -serial file:/tmp/luminary.log

.PHONY: all
all: bootiso

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

.PHONY: disk
disk: | $(BUILD_DIR)
	dd if=/dev/zero of=$(DISK_IMG) bs=512 count=204800
	python3 tools/mkdisk.py $(DISK_IMG)

$(DISK_IMG): | $(BUILD_DIR)
	dd if=/dev/zero of=$(DISK_IMG) bs=512 count=204800
	python3 tools/mkdisk.py $(DISK_IMG)

.PHONY: qemu
qemu: bootiso $(DISK_IMG)
	$(EMULATOR) -cdrom $(ISO_OUT) -hda $(DISK_IMG) -boot d $(QEMU_ARGS)
	cat /tmp/luminary.log

.PHONY: debug
debug: bootiso $(DISK_IMG)
	$(EMULATOR) -cdrom $(ISO_OUT) -hda $(DISK_IMG) -boot d -s -S $(QEMU_ARGS)

.PHONY: gdb
gdb:
	gdb -x tools/gdbinitrc

.PHONY: clean
clean:
	$(MAKE) -C src clean
	$(MAKE) -C userland clean
	rm -rf $(BUILD_DIR)
