CROSS=		i686-elf-
CC=		$(CROSS)gcc
CFLAGS=		-m32 -ffreestanding -nostdlib -std=gnu99 -Wall -Wextra -g -I .
LDFLAGS=	-m32 -T user.ld -nostdlib -lgcc

TARGET=		init.elf

.PHONY: all clean

all: $(TARGET)

$(TARGET): init.o
	$(CC) $(LDFLAGS) -o $@ $^

init.o: init.c syscall.h
	$(CC) $(CFLAGS) -c init.c -o init.o

clean:
	rm -f *.o $(TARGET)
