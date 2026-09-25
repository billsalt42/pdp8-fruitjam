# Convenience targets.  The firmware itself is a normal Pico SDK CMake project.
CC ?= cc

.PHONY: all host firmware test uwm clean
all: host firmware

host: build/pdp8host
build/pdp8host: host/main.c core/pdp8.c core/pdp8.h
	mkdir -p build
	$(CC) -O2 -Wall -Wextra -o $@ host/main.c core/pdp8.c

firmware:
	cmake -S firmware -B build-fw -DCMAKE_BUILD_TYPE=Release
	cmake --build build-fw -j
	cp build-fw/pdp8_fruitjam.uf2 .

# needs python3 + pexpect and a SIMH PDP-8 binary (SIMH_PDP8=/path/to/pdp8)
test: host
	python3 tests/difftest.py | tail -1
	python3 tests/eaetest.py | tail -1
	python3 tests/tss8test.py | tail -1
	python3 tests/uwmtest.py | tail -1

build/palbart: tools/palbart/palbart.c
	mkdir -p build
	$(CC) -O2 -w -o $@ $<

# rebuild sdcard/uwm_rf.dsk + uwm_init.bin from the UWM sources
uwm: build/palbart
	PALBART=$(CURDIR)/build/palbart python3 uwm/make_uwm_disk.py

clean:
	rm -rf build build-fw uwm/build
