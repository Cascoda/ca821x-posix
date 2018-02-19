TARGET_MAIN = app
TARGET_SECTEST = sectest
LIBDIRS = ../
LIBSEARCH := $(addprefix -L,$(LIBDIRS))
LIBS = -lm -lca821x -lstdc++ -lpthread -ldl
INCLUDEDIRS := ./ ../ ../ca821x-api/include ../include
INCLUDEDIRCMD := $(addprefix -I,$(INCLUDEDIRS))
CFLAGS := $(INCLUDEDIRCMD) -std=c99 -g -Wall -pthread

.PHONY: default all clean

default: $(TARGET_MAIN) $(TARGET_SECTEST)
all: default

OBJECTS_MAIN = main.o
OBJECTS_SECTEST = security-test.o
HEADERS := $(foreach DIR,$(INCLUDEDIRS),$(notdir $(wildcard $(DIR)*.h)))

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

.PRECIOUS: $(TARGET_MAIN) $(OBJECTS_MAIN) $(TARGET_SECEST) $(OBJECTS_SECTEST)

$(TARGET_MAIN): $(OBJECTS_MAIN)
	$(CC) $(OBJECTS_MAIN) -Wall $(LIBSEARCH) $(LIBS) -o $@


$(TARGET_SECTEST): $(OBJECTS_SECTEST)
	$(CC) $(OBJECTS_SECTEST) -Wall $(LIBSEARCH) $(LIBS) -o $@

clean:
	-rm -f *.o
	-rm -f $(TARGET_MAIN)
	-rm -f $(TARGET_SECTEST)
