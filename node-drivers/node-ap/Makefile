# Example dynamic main build line
# gcc -fPIC -shared -I/usr/local/include/uvsdk test.c -luvsdk -o test.module

### Build options
CC		=       gcc
INCLUDES        =       -I. -I../../ -I../../libconfuse/src/
CFLAGS		=	-fPIC -shared -O2 $(INCLUDES)

LDFLAGS         =
BIN_TARGET	=	../../modules/node-ap.so

MODULE_SRC      =       $(shell find ./ -name "*.c" -print) ../../mrbfs-pktqueue.c
MODULE_OBJ      =       $(MODULE_SRC:%.c=%.o)
MODULE_HEADERS  =       $(shell find ./ -name "*.h" -print) $(shell find ../../ -name "*.h" -print)
### ap targets
all:    $(BIN_TARGET)

$(BIN_TARGET): $(MODULE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $(BIN_TARGET)

clean: 
	rm -f $(MODULE_OBJ)
	rm -f *.o
	rm -f *.core
	rm -f *~
	rm -f $(BIN_TARGET)
