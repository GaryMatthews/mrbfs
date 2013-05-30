# Example dynamic main build line
# gcc -fPIC -shared -I/usr/local/include/uvsdk test.c -luvsdk -o test.module

### Build options
CC		=       gcc
INCLUDES        =       -I. -I../../ -I../../libconfuse/src/
CFLAGS		=	-fPIC -shared -O2 $(INCLUDES) -D_FILE_OFFSET_BITS=64 -D_REENTRANT -D_GNU_SOURCE -pthread

LDFLAGS         =
BIN_TARGET	=	../../modules/interface-ci2.so

MODULE_SRC      =       $(shell find ./ -name "*.c" -print) ../../mrbfs-pktqueue.c
MODULE_OBJ      =       $(MODULE_SRC:%.c=%.o)

### generic targets
all:	$(MODULE_OBJ)
	ld -shared -soname interface-ci2 -lpthread -o $(BIN_TARGET) -lc $(MODULE_OBJ)

$(BIN_TARGET): $(MODULE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $(BIN_TARGET)

clean: 
	rm -f $(MODULE_OBJ)
	rm -f *.o
	rm -f *.core
	rm -f *~
	rm -f $(BIN_TARGET)
