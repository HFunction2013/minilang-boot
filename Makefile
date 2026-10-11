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

test: $(TARGET)
	@echo "=== Running VM tests ==="
	@for f in tests/*.mil; do \
		echo "--- $$f (VM) ---"; \
		./$(TARGET) run "$$f"; \
	done
