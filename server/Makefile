.PHONY: all clean

CROSS_COMPILE=
CC=$(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Wextra
LIBS=-lpthread

_OBJ = aesdsocket.o
OBJ = $(_OBJ)

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

all: aesdsocket

aesdsocket: $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) $(LIBS)

clean:
	rm -f *.o *~ core aesdsocket
