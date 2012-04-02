LD=ar
LDFLAGS=r
CC=gcc
CFLAGS=-I./include/ -I/usr/include/fuse -I./libconfuse/src/ -O2 -D_FILE_OFFSET_BITS=64 -D_REENTRANT -lfuse -D_GNU_SOURCE
#CFLAGS=-I./include/ -O0 -fPIC -ggdb -DUGLDEBUGSET=2 -ansi
#CFLAGS=-I./include/ -O3  -ansi

MRBFS_HEADERS=$(shell find include/ -name "*.h" -print)

all: mrbfs.c
	$(CC) $(CFLAGS) -o mrbfs mrbfs.c mrbfs-filesys.c mrbfs-log.c ./libconfuse/src/.libs/libconfuse.a
	
clean:
	rm -f *.o
	rm -f *~

