KERNEL_BIN=../src/kernel.bin
TEMPDIR=./.temp

boot.iso: grub.cfg $(KERNEL_BIN)
	rm -rf $(TEMPDIR)
	mkdir -p $(TEMPDIR)/boot/grub
	cp -p $(KERNEL_BIN) $(TEMPDIR)/boot
	cp -p grub.cfg $(TEMPDIR)/boot/grub
	grub2-mkrescue -o $@ $(TEMPDIR)
	rm -rf $(TEMPDIR)

.PHONY: clean
clean:
	rm -f boot.iso
