# tam-clam: the Tamlinux shell. Plain Make; no other build system.
#
#   make            build build/tam-clam
#   make test       unit tests, then the script tests
#   make sanitize   build in build/sanitize with ASan and UBSan, run the tests
#   make install    install to PREFIX (default $HOME/.local)
#   make uninstall  remove what install put there
#   make clean      remove build/
#
# The only dependencies are the C library and libtam, linked statically
# from LIBTAM_PREFIX (see arch/dependencies.md).

PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
LIBTAM_PREFIX ?= $(HOME)/.local

# Where objects and binaries go; make sanitize uses build/sanitize.
B ?= build

VERSION := $(shell tr -d '[:space:]' < VERSION)

CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wvla \
	-D_POSIX_C_SOURCE=200809L -DTC_VERSION='"$(VERSION)"' \
	-Iinclude -I$(LIBTAM_PREFIX)/include \
	-ffile-prefix-map=$(CURDIR)=. -MMD -MP
LIBTAM := $(LIBTAM_PREFIX)/lib/libtam.a

# Everything but main.c, so the unit tests can link it.
CORE_SRCS = src/arena.c src/var.c src/func.c src/shell.c src/signals.c src/input.c src/lex.c \
	src/parse.c src/expand.c src/builtins.c src/exec.c
CORE_OBJS = $(patsubst src/%.c,$(B)/%.o,$(CORE_SRCS))
TESTS = $(B)/tests/test_var $(B)/tests/test_func $(B)/tests/test_lex $(B)/tests/test_parse $(B)/tests/test_expand

all: $(B)/tam-clam

$(B)/tam-clam: $(B)/main.o $(CORE_OBJS) $(LIBTAM)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(B)/main.o $(CORE_OBJS) $(LIBTAM)

$(B)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/tests/test_%: tests/test_%.c $(CORE_OBJS) $(LIBTAM) tests/check.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(CORE_OBJS) $(LIBTAM)

test: $(TESTS) $(B)/tam-clam
	@for t in $(TESTS); do $$t || exit 1; done
	@sh tests/run-scripts.sh $(B)/tam-clam

sanitize:
	$(MAKE) B=build/sanitize \
		CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all' \
		test

install: $(B)/tam-clam
	install -Dm755 $(B)/tam-clam $(DESTDIR)$(BINDIR)/tam-clam

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/tam-clam

clean:
	rm -rf build

.PHONY: all test sanitize install uninstall clean

-include $(shell find $(B) -name '*.d' 2>/dev/null)
