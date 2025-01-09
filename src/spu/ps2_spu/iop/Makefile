IOP_BIN = iop_spu.irx

IOP_INCS +=
IOP_CFLAGS += -Wall
IOP_LDFLAGS += -s

IOP_OBJS = iop_spu.o upsamplers.o hw.o imports.o
IOP_LIBS =

all: $(IOP_BIN)

# A rule to build imports.lst.
imports.o : imports.lst
	echo "#include \"irx_imports.h\"" > build-imports.c
	cat $< >> build-imports.c
	$(IOP_CC) $(IOP_CFLAGS) -c build-imports.c -o $@
	-rm -f build-imports.c

# A rule to build exports.tab.
exports.o : exports.tab
	echo "#include <irx.h>" > build-exports.c
	cat $< >> build-exports.c
	$(IOP_CC) $(IOP_CFLAGS) -c build-exports.c -o $@
	-rm -f build-exports.c
	
clean:
	rm -f $(IOP_BIN) $(IOP_OBJS)

rebuild: clean all

include $(PS2SDK)/Defs.make
include $(PS2SDK)/samples/Makefile.iopglobal
