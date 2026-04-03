CROSS=		i686-elf-
CC=		$(CROSS)gcc
CFLAGS=		-m32 -ffreestanding -nostdlib -std=gnu99 -Wall -Wextra -g -I .
LDFLAGS=	-m32 -T gui.ld -nostdlib -lgcc

TARGET=		gui.elf

.PHONY: all clean

all: $(TARGET)

$(TARGET): gui.o
	$(CC) $(LDFLAGS) -o $@ $^

gui.o: gui.c syscall.h gui.h
	$(CC) $(CFLAGS) -c gui.c -o gui.o

clean:
	rm -f *.o $(TARGET)
