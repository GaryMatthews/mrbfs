LD=ar
LDFLAGS=r
CC=gcc
LDFLAGS=-lfuse -ldl
CFLAGS=-I./include/ -I/usr/include/fuse -I./libconfuse/src/ -O2 -D_FILE_OFFSET_BITS=64 -D_REENTRANT -D_GNU_SOURCE -pthread 

MRBFS_HEADERS=$(shell find include/ -name "*.h" -print)

#LIBCONFUSE_BUILD:=$(shell cd ./libconfuse ; ./configure ; make)


all: libconfuse/src/.libs/libconfuse.a build_core build_drivers

libconfuse/src/.libs/libconfuse.a:
	cd ./libconfuse ; ./configure ; make

build_core:
	$(CC) $(CFLAGS) -o mrbfs mrbfs.c mrbfs-filesys.c mrbfs-log.c ./libconfuse/src/.libs/libconfuse.a $(LDFLAGS)


build_drivers:
	mkdir -p modules
	make -C interface-drivers/interface-ci2
	make -C interface-drivers/interface-dummy
	make -C interface-drivers/interface-xbee
	make -C node-drivers/node-generic
	make -C node-drivers/node-bd42
	make -C node-drivers/node-h2o
	make -C node-drivers/node-rts	
	make -C node-drivers/node-th
	make -C node-drivers/node-wx
	make -C node-drivers/node-ap
	make -C node-drivers/node-acsw
	make -C node-drivers/node-dccm
	make -C node-drivers/node-clockdriver

clean:
	rm -f *.o
	rm -f *~
	rm -f ./modules/*.so
	make -C interface-drivers/interface-ci2 clean
	make -C interface-drivers/interface-dummy clean
	make -C interface-drivers/interface-xbee clean
	make -C node-drivers/node-generic clean
	make -C node-drivers/node-bd42 clean
	make -C node-drivers/node-h2o clean
	make -C node-drivers/node-rts clean
	make -C node-drivers/node-th clean
	make -C node-drivers/node-wx clean
	make -C node-drivers/node-ap clean
	make -C node-drivers/node-acsw clean
	make -C node-drivers/node-dccm clean
	make -C node-drivers/node-clockdriver clean
	