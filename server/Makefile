ifdef CROSS_COMPILE
	CC := $(CROSS_COMPILE)gcc
	else
	CC := gcc
endif

USE_AESD_CHAR_DEVICE ?= 1
CFLAGS = -Wall -Wextra -pthread -g
CFLAGS += -DUSE_AESD_CHAR_DEVICE=$(USE_AESD_CHAR_DEVICE)
LDFLAGS = -pthread

all: aesdsocket
	$(CC) -o aesdsocket aesdsocket.c -Wall -Werror

default: aesdsocket.c
	$(CC) -o aesdsocket aesdsocket.c -Wall -Werror

clean:
	rm -f aesdsocket

