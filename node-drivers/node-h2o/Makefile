# Example dynamic main build line
# gcc -fPIC -shared -I/usr/local/include/uvsdk test.c -luvsdk -o test.module

### Build options
CC    =       gcc
INCLUDES        =       -I. -I../../ -I../../libconfuse/src/ -I../node-common/
CFLAGS      =  -fPIC -shared -O2 $(INCLUDES) -Wall

LDFLAGS         = -lm
BIN_TARGET  =  ../../modules/node-h2o.so

MODULE_SRC      =       $(shell find ./ -name "*.c" -print) ../../mrbfs-pktqueue.c ../node-common/node-helpers.c
MODULE_OBJ      =       $(MODULE_SRC:%.c=%.o)

### rts targets
all:    $(MODULE_OBJ)
	ld -shared -soname node-h2o -lm -o $(BIN_TARGET) -lc $(MODULE_OBJ)

$(BIN_TARGET): $(MODULE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $(BIN_TARGET)

clean:
	rm -f $(MODULE_OBJ)
	rm -f *.o
	rm -f ../node-common/*.o
	rm -f *.core	
	rm -f *~
	rm -f $(BIN_TARGET)
