CROSS=      i686-elf-
CC=         $(CROSS)gcc
AR=         $(CROSS)ar
CFLAGS=     -m32 -ffreestanding -std=gnu99 -Wall -Wextra -Wno-unused-function -g -I .
LDFLAGS=    -m32 -nostdlib -static -lgcc

CRT0=       crt0.o
LIBC=       libc/libc.a
LIBC_SRCS=  libc/string.c libc/stdio.c libc/stdlib.c
LIBC_OBJS=  $(LIBC_SRCS:.c=.o)

TARGETS=    init.elf sh.elf gui.elf term.elf \
            uptime.elf ps.elf kill.elf ls.elf cat.elf stat.elf \
            mount.elf halt.elf exit.elf

.PHONY: all clean
all: $(TARGETS)

# Build the libc archive
$(LIBC): $(LIBC_OBJS)
	$(AR) rcs $@ $^

libc/%.o: libc/%.c libc/string.h libc/stdio.h libc/stdlib.h
	$(CC) $(CFLAGS) -c $< -o $@

# Programs
init.elf: $(CRT0) init.o $(LIBC)
	$(CC) $(LDFLAGS) -o $@ $(CRT0) init.o $(LIBC)

sh.elf: $(CRT0) sh.o $(LIBC)
	$(CC) $(LDFLAGS) -o $@ $(CRT0) sh.o $(LIBC)

gui.elf: $(CRT0) gui.o $(LIBC)
	$(CC) $(LDFLAGS) -o $@ $(CRT0) gui.o $(LIBC)

%.elf: $(CRT0) %.o $(LIBC)
	$(CC) $(LDFLAGS) -o $@ $(CRT0) $*.o $(LIBC)

%.o: %.c syscall.h
	$(CC) $(CFLAGS) -c $< -o $@

gui.o: gui.c gui.h syscall.h
term.o: term.c gui.h syscall.h

clean:
	rm -f *.o $(TARGETS) $(LIBC_OBJS) $(LIBC)
