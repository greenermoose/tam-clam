# tam-clam: the Tamlinux shell. Plain Make; no other build system.
#
#   make            build build/tam-clam
#   make test       unit tests, script tests, POSIX spec suite, fuzz replay
#   make sanitize   build in build/sanitize with ASan and UBSan, run the tests
#   make fuzz       run libFuzzer (if clang installed) or ASan+UBSan mutations
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
CORE_SRCS = src/arena.c src/array.c src/var.c src/func.c src/alias.c src/abbr.c \
	src/hash.c src/options.c src/pattern.c src/regex.c src/arith.c src/glob.c \
	src/brace.c src/shell.c src/signals.c src/trap.c src/bg.c src/jobs.c src/input.c \
	src/lex.c src/parse.c src/expand.c src/cond.c src/lint.c src/builtins.c \
	src/builtins_special.c src/bi_declare.c src/bi_test.c src/bi_printf.c \
	src/builtins_io.c src/builtins_cmd.c src/exec.c src/prompt.c \
	src/history/history.c src/complete/complete.c src/edit/buf.c \
	src/edit/keymap.c src/edit/highlight.c src/edit/render.c src/edit/edit.c
CORE_OBJS = $(patsubst src/%.c,$(B)/%.o,$(CORE_SRCS))
TESTS = $(B)/tests/test_array $(B)/tests/test_var $(B)/tests/test_func $(B)/tests/test_alias \
	$(B)/tests/test_abbr $(B)/tests/test_hash $(B)/tests/test_options $(B)/tests/test_test \
	$(B)/tests/test_pattern $(B)/tests/test_regex $(B)/tests/test_brace \
	$(B)/tests/test_cond $(B)/tests/test_arith $(B)/tests/test_glob \
	$(B)/tests/test_procsub $(B)/tests/test_strict $(B)/tests/test_trap $(B)/tests/test_lex $(B)/tests/test_parse \
	$(B)/tests/test_expand $(B)/tests/test_lint $(B)/tests/test_edit \
	$(B)/tests/test_history $(B)/tests/test_jobs $(B)/tests/test_complete \
	$(B)/tests/test_highlight $(B)/tests/test_prompt
PTY_TESTS = $(B)/tests/pty/test_pty

FUZZ_BINS = $(B)/fuzz/fuzz_parse $(B)/fuzz/fuzz_expand

all: $(B)/tam-clam

$(B)/tam-clam: $(B)/main.o $(CORE_OBJS) $(LIBTAM)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(B)/main.o $(CORE_OBJS) $(LIBTAM)

$(B)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/tests/test_%: tests/test_%.c $(CORE_OBJS) $(LIBTAM) tests/check.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(CORE_OBJS) $(LIBTAM)

$(B)/tests/pty/test_pty: tests/pty/test_pty.c tests/check.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(B)/fuzz/fuzz_%: tests/fuzz/fuzz_%.c tests/fuzz/fuzz_main.c $(CORE_OBJS) $(LIBTAM)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< tests/fuzz/fuzz_main.c $(CORE_OBJS) $(LIBTAM)

test: $(TESTS) $(PTY_TESTS) $(FUZZ_BINS) $(B)/tam-clam
	@for t in $(TESTS); do $$t || exit 1; done
	@$(B)/tests/pty/test_pty $(B)/tam-clam
	@sh tests/run-scripts.sh $(B)/tam-clam
	@python3 tests/spec/run-spec.py $(B)/tam-clam
	@$(B)/fuzz/fuzz_parse tests/fuzz/corpus/parse
	@$(B)/fuzz/fuzz_expand tests/fuzz/corpus/expand

sanitize:
	$(MAKE) B=build/sanitize \
		CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all' \
		test

FUZZ_ITERS ?= 10000
fuzz:
	@if command -v clang >/dev/null 2>&1; then \
		mkdir -p build/fuzz; \
		clang -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
			-std=c11 -D_POSIX_C_SOURCE=200809L -DTC_VERSION='"$(VERSION)"' \
			-Iinclude -I$(LIBTAM_PREFIX)/include \
			tests/fuzz/fuzz_parse.c $(CORE_SRCS) $(LIBTAM) -o build/fuzz/libfuzz_parse; \
		clang -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
			-std=c11 -D_POSIX_C_SOURCE=200809L -DTC_VERSION='"$(VERSION)"' \
			-Iinclude -I$(LIBTAM_PREFIX)/include \
			tests/fuzz/fuzz_expand.c $(CORE_SRCS) $(LIBTAM) -o build/fuzz/libfuzz_expand; \
		build/fuzz/libfuzz_parse -runs=$(FUZZ_ITERS) tests/fuzz/corpus/parse; \
		build/fuzz/libfuzz_expand -runs=$(FUZZ_ITERS) tests/fuzz/corpus/expand; \
	else \
		$(MAKE) B=build/sanitize \
			CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all' \
			build/sanitize/fuzz/fuzz_parse build/sanitize/fuzz/fuzz_expand; \
		build/sanitize/fuzz/fuzz_parse --mutate $(FUZZ_ITERS) tests/fuzz/corpus/parse; \
		build/sanitize/fuzz/fuzz_expand --mutate $(FUZZ_ITERS) tests/fuzz/corpus/expand; \
	fi

install: $(B)/tam-clam
	install -Dm755 $(B)/tam-clam $(DESTDIR)$(BINDIR)/tam-clam
	install -Dm755 tools/tam-clam-gen-comp $(DESTDIR)$(BINDIR)/tam-clam-gen-comp
	@for f in share/completions/*.comp; do \
		install -Dm644 "$$f" "$(DESTDIR)$(PREFIX)/share/tam-clam/completions/$$(basename "$$f")"; \
	done

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/tam-clam $(DESTDIR)$(BINDIR)/tam-clam-gen-comp
	rm -rf $(DESTDIR)$(PREFIX)/share/tam-clam/completions

clean:
	rm -rf build

.PHONY: all test sanitize fuzz install uninstall clean

-include $(shell find $(B) -name '*.d' 2>/dev/null)
