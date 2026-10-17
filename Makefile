CC      ?= gcc
CFLAGS  = -Wall -Wextra -O1 -g -fPIC
TESTCF  = -Wall -Wextra -g -pthread

all: build/libpdp.so tests/bin/basic

build/libpdp.so: src/libpdp.c src/pdp.h
	@mkdir -p build
	$(CC) $(CFLAGS) -shared -o $@ src/libpdp.c -ldl -pthread

tests/bin/basic: tests/basic.c
	@mkdir -p tests/bin
	$(CC) $(TESTCF) -o $@ tests/basic.c

test: all
	@bash tests/check_basic.sh

clean:
	rm -rf build tests/bin

.PHONY: all test clean
