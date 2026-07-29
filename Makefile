# Convenience wrapper for people who do not want CMake.
CXX      ?= g++
CPPFLAGS ?=
CXXSTD   ?= -std=c++17
CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic
LDFLAGS  ?=
LDLIBS   ?=
PYTHON   ?= python3
PREFIX   ?= /usr/local
VERSION  ?= $(shell sed -n 's/^project(lapse VERSION \([^ ]*\) LANGUAGES CXX)/\1/p' CMakeLists.txt)

ifeq ($(strip $(VERSION)),)
$(error Could not read the lapse version from CMakeLists.txt)
endif

ifeq ($(OS),Windows_NT)
LDLIBS += -ladvapi32
endif

lapse: Makefile CMakeLists.txt src/lapse.cpp src/sha256.cpp src/sha256.hpp
	$(CXX) $(CPPFLAGS) -DLAPSE_VERSION=\"$(VERSION)\" $(CXXSTD) $(CXXFLAGS) -o $@ \
		src/lapse.cpp src/sha256.cpp $(LDFLAGS) $(LDLIBS)

.PHONY: install test clean
install: lapse
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 lapse "$(DESTDIR)$(PREFIX)/bin/lapse"

test: lapse
	$(PYTHON) tests/integration.py ./lapse

clean:
	rm -f lapse
