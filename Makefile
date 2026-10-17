CC      ?= gcc
CFLAGS  = -Wall -Wextra -O1 -g -fPIC
TESTCF  = -Wall -Wextra -g -pthread -fPIE -pie
# PIE: site offsets assume it (see pdp.h). No -O so call-site line numbers stay exact.

all: build/libpdp.so build/pdp-analyze build/pdp tests/bin/basic tests/bin/overflow tests/bin/bank tests/bin/bank_fixed tests/bin/sem_sq tests/bin/gate tests/bin/philo_naive tests/bin/philo_ordered tests/bin/prodcons tests/bin/recursive

build/libpdp.so: src/libpdp.c src/pdp.h
	@mkdir -p build
	$(CC) $(CFLAGS) -shared -o $@ src/libpdp.c -ldl -pthread

build/pdp-analyze: src/analyze.c src/report.c src/analyze.h
	@mkdir -p build
	$(CC) -Wall -Wextra -O1 -g -o $@ src/analyze.c src/report.c

build/pdp: src/pdp
	@mkdir -p build
	cp src/pdp $@ && chmod +x $@

tests/bin/%: tests/%.c
	@mkdir -p tests/bin
	$(CC) $(TESTCF) -o $@ $<

test: all
	@bash tests/check_basic.sh
	@bash tests/check_analyze.sh
	@bash tests/check_suite.sh
	@bash tests/check_confirm.sh

clean:
	rm -rf build tests/bin

.PHONY: all test clean
