CC      ?= gcc
CFLAGS  = -Wall -Wextra -O1 -g -fPIC
TESTCF  = -Wall -Wextra -g -pthread -fPIE -pie   # PIE: site offsets assume it (see pdp.h)

all: build/libpdp.so build/pdp-analyze build/pdp tests/bin/basic tests/bin/overflow tests/bin/bank

build/libpdp.so: src/libpdp.c src/pdp.h
	@mkdir -p build
	$(CC) $(CFLAGS) -shared -o $@ src/libpdp.c -ldl -pthread

tests/bin/basic: tests/basic.c
	@mkdir -p tests/bin
	$(CC) $(TESTCF) -o $@ tests/basic.c

build/pdp-analyze: src/analyze.c src/report.c src/analyze.h
	@mkdir -p build
	$(CC) -Wall -Wextra -O1 -g -o $@ src/analyze.c src/report.c

build/pdp: src/pdp
	@mkdir -p build
	cp src/pdp $@ && chmod +x $@

tests/bin/bank: tests/bank.c
	@mkdir -p tests/bin
	$(CC) $(TESTCF) -o $@ tests/bank.c

tests/bin/overflow: tests/overflow.c
	@mkdir -p tests/bin
	$(CC) $(TESTCF) -o $@ tests/overflow.c

test: all
	@bash tests/check_basic.sh
	@bash tests/check_analyze.sh

clean:
	rm -rf build tests/bin

.PHONY: all test clean
