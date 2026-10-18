CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
TARGET := network-simulator
SOURCES := server.c network.c

.PHONY: all build run test clean

all: build

build: $(TARGET)

$(TARGET): $(SOURCES) network.h
	$(CC) $(CFLAGS) $(SOURCES) -o $@

run: build
	./$(TARGET)

test: build
	sh tests/test_api.sh

clean:
	rm -f $(TARGET)
