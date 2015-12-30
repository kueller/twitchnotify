SOURCE = twitchnotify.c
BINARY = twitchnotify
CC     = gcc
CFLAGS = -Wall -lcurl -lnotify
CONFIG = pkg-config --cflags --libs libnotify gdk-pixbuf-2.0
OBJ    = ${SRC:.c=.o}

all: ${BINARY}

${BINARY}:
	${CC} ${SOURCE} -o ${BINARY} ${CFLAGS} `${CONFIG}`

install: ${BINARY}
	cp ${BINARY} /usr/bin
	mkdir -p /usr/share/twitchnotify
	cp GlitchIcon_purple.png /usr/share/twitchnotify

clean:
	rm -f ${BINARY}

.PHONY: clean
