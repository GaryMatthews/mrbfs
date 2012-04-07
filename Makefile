LD=ar
LDFLAGS=r
CC=gcc
CFLAGS=-I./include/ -I/usr/include/fuse -I./libconfuse/src/ -O2 -D_FILE_OFFSET_BITS=64 -D_REENTRANT -lfuse -D_GNU_SOURCE
#CFLAGS=-I./include/ -O0 -fPIC -ggdb -DUGLDEBUGSET=2 -ansi
#CFLAGS=-I./include/ -O3  -ansi

MRBFS_HEADERS=$(shell find include/ -name "*.h" -print)

LIBCONFUSE_BUILD:=$(shell cd ./libconfuse ; ./configure ; make)


all: build_core build_drivers

libconfuse/src/.libs/libconfuse.a: ./libconfuse/src/.libs/libconfuse.a
	LIBCONFUSE_BUILD
	

build_core:
	$(CC) $(CFLAGS) -o mrbfs mrbfs.c mrbfs-filesys.c mrbfs-log.c ./libconfuse/src/.libs/libconfuse.a


build_drivers:
	mkdir -p modules
	make -C interface-drivers/interface-ci2
	make -C interface-drivers/interface-dummy
	make -C node-drivers/node-generic
	make -C node-drivers/node-bd42
	

clean:
	rm -f *.o
	rm -f *~

