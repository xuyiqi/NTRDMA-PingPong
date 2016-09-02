CC=gcc
CFLAGS=-c -Wall -g
LDFLAGS=-Llibrdmacm -libverbs -lpthread

SOURCES=rc_pingpong.c pingpong.c
OBJECTS=$(SOURCES:.cpp=.o)

EXECUTABLE=pp

all: $(SOURCES) $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

.cpp.o:
	$(CC) $(CFLAGS) $< -o $@

clean:
	$(RM) $(OBJECTS)
