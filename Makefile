TARGET_LIB = libdl.a
OBJS       = source/dl.o
INCLUDES   = include

PREFIX  ?= ${VITASDK}/arm-vita-eabi
CC      = arm-vita-eabi-gcc
AR      = arm-vita-eabi-ar
CFLAGS  = -Wl,-q -Wall -O2 -I$(INCLUDES)
ASFLAGS = $(CFLAGS)

all: $(TARGET_LIB)

$(TARGET_LIB): $(OBJS)
	$(AR) -rc $@ $^

clean:
	rm -rf $(TARGET_LIB) $(OBJS)

install: $(TARGET_LIB)
	@mkdir -p $(DESTDIR)$(PREFIX)/lib/
	cp $(TARGET_LIB) $(DESTDIR)$(PREFIX)/lib/
	@mkdir -p $(DESTDIR)$(PREFIX)/include/
	cp include/dlfcn.h $(DESTDIR)$(PREFIX)/include/
