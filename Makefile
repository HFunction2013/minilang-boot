CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=gnu11
SRCS = value.c lexer.c parser.c bytecode.c vm.c c_gen.c milc_io.c native.c runtime.c main.c
OBJS = $(SRCS:.c=.o)
TARGET = minilang

# run -e loads user extensions with dlopen()/LoadLibrary(); the VM/runtime
# symbols those extensions call (ml_opaque_ptr, ml_print, ...) must be exported
# from the main binary. Linux/macOS use --export-dynamic; Windows (MinGW) has
# no such flag and exports every symbol explicitly.
ifeq ($(OS),Windows_NT)
EXPORT_LDFLAGS = -Wl,--export-all-symbols
else
UNAME := $(shell uname)
ifeq ($(UNAME),Linux)
EXPORT_LDFLAGS = -Wl,--export-dynamic -ldl
else
EXPORT_LDFLAGS = -Wl,-export_dynamic
endif
endif

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(EXPORT_LDFLAGS) -o $@ $^

%.o: %.c minilang.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) *.o tests/*.o tests/*_native
	rm -f tests/*.c

# The .mil test programs live in the sibling minilang-compiler checkout since
# the toolchain split (2026-10). Prefer a local tests/ dir when present, then
# fall back to ../minilang-compiler/tests so `make test` works out of the box.
# MINILANG_DIR is pointed at the repo that owns the test programs so their
# `require ... from <module>` resolves against that repo's syslib/.
test: $(TARGET)
	@echo "=== Running VM tests ==="
	@if [ -d tests ]; then \
		TDIR="tests"; \
		export MINILANG_DIR="$$(pwd)"; \
	elif [ -d ../minilang-compiler/tests ]; then \
		TDIR="../minilang-compiler/tests"; \
		export MINILANG_DIR="$$(cd ../minilang-compiler && pwd)"; \
	else \
		echo "No tests found (looked for tests/ and ../minilang-compiler/tests)"; \
		exit 1; \
	fi; \
	for f in "$$TDIR"/*.mil; do \
		[ -f "$$f" ] || continue; \
		echo "--- $$f (VM) ---"; \
		./$(TARGET) run "$$f"; \
	done
