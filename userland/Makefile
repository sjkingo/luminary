CROSS=      i686-elf-
CC=         $(CROSS)gcc
AR=         $(CROSS)ar
CFLAGS=     -m32 -ffreestanding -std=gnu99 -Wall -Wextra -Wno-unused-function -g -I .
LDFLAGS=    -m32 -nostdlib -static -lgcc

CRT0=       crt0.o
LIBC=       libc/libc.a
LIBC_SRCS=  libc/string.c libc/stdio.c libc/stdlib.c
LIBC_OBJS=  $(LIBC_SRCS:.c=.o)

# BIN_DIR is set by the root Makefile to _build/rootfs/bin
BIN_DIR    ?= .

PROGS=      init sh gui term uptime ps kill ls cat stat mkdir mount umount halt reboot wallpaper busy mv
TARGETS=    $(addprefix $(BIN_DIR)/,$(PROGS))

.PHONY: all clean
all: $(TARGETS)

# Build the libc archive
$(LIBC): $(LIBC_OBJS)
	$(AR) rcs $@ $^

libc/%.o: libc/%.c libc/string.h libc/stdio.h libc/stdlib.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/%: $(CRT0) %.o $(LIBC)
	$(CC) $(LDFLAGS) -o $@ $(CRT0) $*.o $(LIBC)

# Prevent make's built-in .c -> executable rule from firing
%: %.c

# Compile rules
%.o: %.c syscall.h
	$(CC) $(CFLAGS) -c $< -o $@


clean:
	rm -f *.o $(LIBC_OBJS) $(LIBC)
