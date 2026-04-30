ifdef CROSS_COMPILE
	CC := $(CROSS_COMPILE)gcc
	else
	CC := gcc
endif

all: writer
	$(CC) -o writer writer.c

default: writer.c
	$(CC) -o writer writer.c

clean:
	rm -f writer
