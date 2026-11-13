CC = gcc
CPPFLAGS ?= -I.
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror -Wshadow -fno-builtin
LDFLAGS ?=
LDLIBS ?= -pthread

.PHONY: all check clean

all: build/liballocator.so build/liballocator_with_stats.so build/test_allocator

build:
	mkdir -p $@

# Each library compiles exactly one allocator implementation.
build/liballocator.so: allocator.c allocator.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -fvisibility=hidden -shared $(LDFLAGS) $< -o $@ $(LDLIBS)

build/liballocator_with_stats.so: allocat_with_stats.c allocator.h allocator_stats.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -fvisibility=hidden -shared $(LDFLAGS) $< -o $@ $(LDLIBS)

# Weak diagnostic symbols let this executable also run with glibc alone.
build/test_allocator: test_allocator.c allocator_stats.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< -o $@ $(LDLIBS)

check: all
	env -u LD_PRELOAD -u ALLOC_STRATEGY ./build/test_allocator --system
	env -u LD_PRELOAD -u ALLOC_STRATEGY ./build/test_allocator --system --oom-only
	@set -e; \
	for strategy in first best next; do \
		env LD_PRELOAD="$(CURDIR)/build/liballocator_with_stats.so" \
			ALLOC_STRATEGY="$$strategy" ./build/test_allocator --require-custom; \
		env LD_PRELOAD="$(CURDIR)/build/liballocator_with_stats.so" \
			ALLOC_STRATEGY="$$strategy" ./build/test_allocator --require-custom --oom-only; \
	done

clean:
	$(RM) -r build
