KERNEL_BIN=../src/kernel.bin
USER_ELF=../user/init.elf
TEMPDIR=./.temp
GRUB_MKRESCUE=$(shell command -v grub2-mkrescue || command -v i686-elf-grub-mkrescue || command -v grub-mkrescue)

boot.iso: grub.cfg $(KERNEL_BIN) $(USER_ELF)
	rm -rf $(TEMPDIR)
	mkdir -p $(TEMPDIR)/boot/grub
	cp -p $(KERNEL_BIN) $(TEMPDIR)/boot
	cp -p $(USER_ELF) $(TEMPDIR)/boot
	cp -p grub.cfg $(TEMPDIR)/boot/grub
	$(GRUB_MKRESCUE) -o $@ $(TEMPDIR) 2>/dev/null
	rm -rf $(TEMPDIR)

.PHONY: clean
clean:
	rm -f boot.iso
