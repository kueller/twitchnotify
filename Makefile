SOURCE = twitchnotify.c
BINARY = twitchnotify
CC     = gcc
CFLAGS = -Wall -lcurl -lnotify
CONFIG = pkg-config --cflags --libs libnotify
OBJ    = ${SRC:.c=.o}

all: ${BINARY}

${BINARY}:
	${CC} ${CFLAGS} `${CONFIG}` ${SOURCE} -o ${BINARY}

install: ${BINARY}
	cp ${BINARY} /usr/bin

clean:
	rm -f ${BINARY}

.PHONY: clean
