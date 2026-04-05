BUILD_DIR   = _build
KERNEL_BIN  = $(BUILD_DIR)/kernel.bin
INITRD_IMG  = $(BUILD_DIR)/initrd.img
ROOTFS_DIR  = $(BUILD_DIR)/rootfs
BIN_DIR     = $(ROOTFS_DIR)/bin
ISO_OUT     = $(BUILD_DIR)/boot.iso
DISK_IMG    = $(BUILD_DIR)/disk.img

EMULATOR    = qemu-system-i386
QEMU_ARGS   = -m 512 -net nic,model=rtl8139 -net user -serial file:/tmp/luminary.log
DISK_ARGS   = -hda $(DISK_IMG)

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

.PHONY: qemu
qemu: kernel initrd
	$(EMULATOR) -kernel $(KERNEL_BIN) -initrd $(INITRD_IMG) $(QEMU_ARGS)

.PHONY: disk
disk: | $(BUILD_DIR)
	dd if=/dev/zero of=$(DISK_IMG) bs=512 count=204800
	printf '\x55\xaa' | dd of=$(DISK_IMG) bs=1 seek=510 conv=notrunc
	printf '\x80\x00\x00\x00\x83\x00\x00\x00\x00\x08\x00\x00\x00\x18\x03\x00' \
		| dd of=$(DISK_IMG) bs=1 seek=446 conv=notrunc
	@echo "disk: created $(DISK_IMG) (100MB, MBR with partition 1 at LBA 2048, 202752 sectors)"

.PHONY: qemucd
qemucd: bootiso
	$(EMULATOR) -cdrom $(ISO_OUT) $(QEMU_ARGS)
	cat /tmp/luminary.log

.PHONY: qemucd-disk
qemucd-disk: bootiso $(DISK_IMG)
	$(EMULATOR) -cdrom $(ISO_OUT) $(QEMU_ARGS) $(DISK_ARGS)
	cat /tmp/luminary.log

.PHONY: qemucd-debug
qemucd-debug: bootiso
	$(EMULATOR) -cdrom $(ISO_OUT) $(QEMU_ARGS) -d cpu_reset -no-reboot 2>&1 | tee /tmp/luminary-reset.log
	cat /tmp/luminary.log

.PHONY: console
console: kernel initrd
	$(EMULATOR) -kernel $(KERNEL_BIN) -initrd $(INITRD_IMG) -nographic $(QEMU_ARGS) &
	sleep 2
	pkill qemu-system-*

.PHONY: debug
debug: kernel initrd
	$(EMULATOR) -kernel $(KERNEL_BIN) -initrd $(INITRD_IMG) -s -S $(QEMU_ARGS)

.PHONY: gdb
gdb:
	gdb -x tools/gdbinitrc

.PHONY: clean
clean:
	$(MAKE) -C src clean
	$(MAKE) -C userland clean
	rm -rf $(BUILD_DIR)
