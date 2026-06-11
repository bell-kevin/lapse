# Convenience wrapper for people who do not want CMake.
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
PREFIX   ?= /usr/local

lapse: src/lapse.cpp src/sha256.cpp src/sha256.hpp
	$(CXX) $(CXXFLAGS) -o $@ src/lapse.cpp src/sha256.cpp

.PHONY: install test clean
install: lapse
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 lapse $(DESTDIR)$(PREFIX)/bin/lapse

test: lapse
	./tests/smoke.sh ./lapse

clean:
	rm -f lapse
