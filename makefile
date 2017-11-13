TARGET = libca821x.a
LIBS = -lm
CFLAGS = -g -Wall -pthread -std=c99 -D_POSIX_C_SOURCE=199309L
INCLUDEDIRS = ca821x-api/include/ include/
SOURCEDIRS = kernel-exchange/ usb-exchange/ util/ source/
SUBDIRS = ca821x-api

.PHONY: default all clean subdirs $(SUBDIRS)

default: $(TARGET)
all: default

INCLUDES = $(foreach dir, $(INCLUDEDIRS), -I$(dir))
OBJECTS = $(patsubst %.c, %.o, $(wildcard $(addsuffix *.c,$(SOURCEDIRS))))
HEADERS = $(wildcard $(addsuffix *.h,$(INCLUDEDIRS)))

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@ $(INCLUDES)

.PRECIOUS: $(TARGET) $(OBJECTS)

$(TARGET): $(OBJECTS) subdirs
	cp ca821x-api/libca821x.a ./libca821x.a
	$(AR) -x libca821x.a
	$(AR) rcs $(TARGET) $(OBJECTS)

clean:
	-rm -f $(addsuffix *.o,$(SOURCEDIRS))
	-rm -f $(TARGET)

subdirs: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@