CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=gnu11
SRCS = value.c lexer.c parser.c bytecode.c vm.c llvm_gen.c milc_io.c native.c main.c
OBJS = $(SRCS:.c=.o)
TARGET = minilang

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c minilang.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) *.ll *.o tests/*.ll tests/*.o tests/*_native

# The .mil test programs live in the sibling minilang-compiler checkout since
# the toolchain split (2026-10). Prefer a local tests/ dir when present, then
# fall back to ../minilang-compiler/tests so `make test` works out of the box.
test: $(TARGET)
	@echo "=== Running VM tests ==="
	@if [ -d tests ]; then \
		TDIR="tests"; \
	elif [ -d ../minilang-compiler/tests ]; then \
		TDIR="../minilang-compiler/tests"; \
	else \
		echo "No tests found (looked for tests/ and ../minilang-compiler/tests)"; \
		exit 1; \
	fi; \
	for f in "$$TDIR"/*.mil; do \
		[ -f "$$f" ] || continue; \
		echo "--- $$f (VM) ---"; \
		./$(TARGET) run "$$f"; \
	done
