KERNEL_BIN ?= ../src/kernel.bin
INITRD_IMG ?= ../initrd.img
ISO_OUT    ?= boot.iso

GRUB_MKRESCUE=$(shell command -v grub2-mkrescue || command -v i686-elf-grub-mkrescue || command -v grub-mkrescue)

$(ISO_OUT): grub.cfg $(KERNEL_BIN) $(INITRD_IMG)
	rm -rf .tmp_iso
	mkdir -p .tmp_iso/boot/grub
	cp -p $(KERNEL_BIN) .tmp_iso/boot/
	cp -p $(INITRD_IMG) .tmp_iso/boot/
	cp -p grub.cfg .tmp_iso/boot/grub/
	$(GRUB_MKRESCUE) -o $@ .tmp_iso 2>/dev/null
	rm -rf .tmp_iso

.PHONY: clean
clean:
	rm -f boot.iso
	rm -rf .tmp_iso
