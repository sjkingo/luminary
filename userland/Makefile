CROSS=      i686-elf-
CC=         $(CROSS)gcc
AR=         $(CROSS)ar
CFLAGS=     -m32 -ffreestanding -std=gnu99 -Wall -Wextra -Wno-unused-function -g -I . -MMD -MP
LDFLAGS=    -m32 -nostdlib -static -lgcc

CRT0=       crt0.o
LIBC=       libc/libc.a
LIBC_SRCS=  libc/string.c libc/stdio.c libc/stdlib.c libc/termemu.c
LIBC_OBJS=  $(LIBC_SRCS:.c=.o)

# BIN_DIR is set by the root Makefile to _build/rootfs/bin
BIN_DIR    ?= .

PROGS=      init sh gui term fbcon uptime ps kill ls cat stat mkdir mount umount halt reboot wallpaper busy mv
TARGETS=    $(addprefix $(BIN_DIR)/,$(PROGS))

.PHONY: all clean
all: $(TARGETS)

# Build the libc archive
$(LIBC): $(LIBC_OBJS)
	$(AR) rcs $@ $^

libc/%.o: libc/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/%: $(CRT0) %.o $(LIBC)
	$(CC) $(LDFLAGS) -o $@ $(CRT0) $*.o $(LIBC)

# Prevent make's built-in .c -> executable rule from firing
%: %.c

# Compile rules
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

DEPFILES = $(wildcard *.d libc/*.d)
-include $(DEPFILES)

clean:
	rm -f *.o *.d $(LIBC_OBJS) $(patsubst %.o,%.d,$(LIBC_OBJS)) $(LIBC)
