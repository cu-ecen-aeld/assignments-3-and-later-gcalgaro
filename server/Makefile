ifdef CROSS_COMPILE
	CC := $(CROSS_COMPILE)gcc
	else
	CC := gcc
endif

all: aesdsocket
	$(CC) -o aesdsocket aesdsocket.c -Wall -Werror

default: aesdsocket.c
	$(CC) -o aesdsocket aesdsocket.c -Wall -Werror

clean:
	rm -f aesdsocket
