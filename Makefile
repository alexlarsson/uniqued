all: uniqued unique-client

uniqued: uniqued.c
	gcc uniqued.c `pkg-config --cflags --libs gio-unix-2.0` -Wall -O2 -g -o uniqued

unique-client: unique-client.c unique-bytes.h unique-bytes.c
	gcc unique-bytes.c unique-client.c `pkg-config --cflags --libs gio-unix-2.0` -Wall -O2 -g -o unique-client
