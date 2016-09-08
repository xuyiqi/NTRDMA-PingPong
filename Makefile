CC=gcc
CFLAGS=-c -Wall -g -D_GNU_SOURCE
LDFLAGS=-Llibrdmacm -libverbs -lpthread

SOURCES=rc_pingpong.c pingpong.c
OBJECTS=$(SOURCES:.c=.o)

EXECUTABLE=pp

all: $(SOURCES) $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

.cpp.o:
	$(CC) $(CFLAGS) $< -o $@

clean:
	$(RM) $(OBJECTS)
