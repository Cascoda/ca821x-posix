TARGET = libca821x.a
LIBS = -lm
CFLAGS = -g -Wall -pthread -std=c99 -D_POSIX_C_SOURCE=199309L
INCLUDEDIR = ca821x-api/include/
SOURCEDIRS = kernel-exchange/ usb-exchange/ util/
SUBDIRS = ca821x-api

.PHONY: default all clean subdirs $(SUBDIRS)

default: $(TARGET)
all: default

OBJECTS = $(patsubst %.c, %.o, $(wildcard $(addsuffix *.c,$(SOURCEDIRS))))
HEADERS = $(wildcard $(INCLUDEDIR),*.h)

%.o: %.c $(HEADERS) subdirs
	$(CC) $(CFLAGS) -c $< -o $@ -I $(INCLUDEDIR)

.PRECIOUS: $(TARGET) $(OBJECTS)

$(TARGET): $(OBJECTS)
	cp ca821x-api/libca821x.a ./libca821x.a
	$(AR) -x libca821x.a
	$(AR) rcs $(TARGET) $(OBJECTS)

clean:
	-rm -f $(addsuffix *.o,$(SOURCEDIRS))
	-rm -f $(TARGET)

subdirs: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@