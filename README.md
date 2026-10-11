# minilang-boot

The **handwritten C bootstrap toolchain** for the minilang language. This is the
small C program that can lex, parse, compile to bytecode, run the stack VM, and
emit LLVM IR — it is the seed from which the self-hosted minilang compiler
(`minilang-compiler`) is bootstrapped.

Split out of [`minilang-compiler`](https://github.com/HFunction2013/minilang-compiler).

## Contents

| File | Role |
|------|------|
| `minilang.h` | shared types: `Value`, AST nodes, bytecode opcodes, VM, native API |
| `lexer.c` | tokenizer (`var/func/if/while/native/...`) |
| `parser.c` | recursive-descent parser + `require` module system |
| `bytecode.c` | AST → stack bytecode compiler |
| `vm.c` | stack-based VM + `OP_NATIVE` C-dispatch |
| `value.c` | value operations (int/string/array arithmetic) |
| `milc_io.c` | `.milc` binary (magic `!milc`) serializer |
| `llvm_gen.c` | AST → LLVM IR text |
| `runtime.c` | C runtime linked into native executables (LLVM backend) |
| `native.c` | registry + built-in **native methods** (`sqrt`, `abs`, `time`, `getenv`, …) |
| `main.c` | CLI: `run / bytecode / dump-text / llvm / build -b|-e / repl / self-test` |
| `ir_compile.py` | (legacy) llvmlite-based `.ll`→`.o`; replaced by LLVM project binaries |

## Build

```bash
make
```

Produces `./minilang` (the boot compiler + VM). The standard library is expected
in `./syslib` — add it as a submodule:

```bash
git submodule add https://github.com/HFunction2013/minilang-syslib syslib
```

## Native methods

A minilang program declares a C function without a body:

```minilang
require sqrt from builtin;   // builtin.mil lives in syslib
println toString(sqrt(144)); // 12
```

The VM looks the name up in `native.c` (`ml_native_register`) and calls the C
pointer. New natives are added in `native.c` — no interpreter changes needed.

## Linking native executables

`build -e <file>` emits LLVM IR (`.ll`), then uses **LLVM project binaries**
(`llc` to assemble an object, `clang`/`ld.lld` to link `runtime.c`) instead of
Python llvmlite. Point `LLVM_DIR` at an LLVM install, or have `llc`/`clang` on
`PATH`.

## License

GPL-3.0.
