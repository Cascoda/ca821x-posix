TARGET = libca8210.a
LIBS = -lm
CFLAGS = -g -Wall -pthread -std=c99
INCLUDEDIR = ca821x-api/include/
SOURCEDIR = kernel-exchange/ usb-exchange/
SUBDIRS = ca821x-api

.PHONY: default all clean subdirs $(SUBDIRS)

default: $(TARGET)
all: default

OBJECTS = $(patsubst %.c, %.o, $(wildcard $(SOURCEDIR)*.c))
HEADERS = $(wildcard $(INCLUDEDIR),*.h)

%.o: %.c $(HEADERS) subdirs
	$(CC) $(CFLAGS) -c $< -o $@ -I $(INCLUDEDIR)

.PRECIOUS: $(TARGET) $(OBJECTS)

$(TARGET): $(OBJECTS)
	cp ca821x-api/libca821x.a ./libca821x.a
	$(AR) -x libca821x.a
	$(AR) rcs $(TARGET) *.o

clean:
	-rm -f $(addsuffix *.o,$(SOURCEDIR))
	-rm -f $(TARGET)

subdirs: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@