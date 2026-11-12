# minilang-boot

The **handwritten C bootstrap toolchain** for the minilang language. This is the
small C program that can lex, parse, compile to bytecode, run the stack VM, and
emit C source for a native build — it is the seed from which the self-hosted
minilang compiler (`minilang-compiler`) is bootstrapped.

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
| `c_gen.c` | AST → C source (native backend) |
| `runtime.c` | C runtime linked into native executables (C backend) |
| `native.c` | registry + built-in **native methods** (`sqrt`, `abs`, `time`, `getenv`, …) |
| `main.c` | CLI: `run / bytecode / dump-text / c / build -b|-e / repl / self-test` |

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

`build -e <file>` emits a C source file (`<base>.c`) and compiles it with the
system C compiler at `-O2`, linking `runtime.c`:

```
cc -O2 -I<runtime dir> <base>.c runtime.c [prebuilt extensions] -o <bin>
```

The compiler driver comes from `$CC` (Unix default `cc`, Windows default
`clang`); no LLVM/llc/llvmlite step is involved.

## User C extensions

A `.mil` file can declare a native that a C implementation supplied on the
command line provides:

```minilang
native func scale(x);
```

Extensions must be **precompiled yourself** — the toolchain never compiles
extension `.c` on the fly. Pass a prebuilt shared library to load while
interpreting:

```bash
./minilang run -e prog.mil ext.so -- args   # .dll on Windows
```

Or link a precompiled object/archive/shared library into a native executable:

```bash
./minilang build -e prog.mil ext.o
```

Raw `.c` sources are rejected. `--dynamic` is accepted for compatibility but
no longer changes the link mode.

Two signatures are accepted: `Value ml_native_<name>(Value ...)` (packed ABI,
bridged to the VM value) and `MLValue ml_vm_native_<name>(void*, MLValue*, int)`
(VM style, registered directly). The packed bridge supports arity 0–3; arrays
cross the ABI best-effort. The extension API — `ml_nil`, `ml_is_int`,
`ml_mkarray`, `ml_opaque_create`, ... — is documented in `minilang_runtime.h`.

## Windows

MSVC and MinGW are both supported (everything branches on `_WIN32`; MSVC builds
through CMake: `cmake -B build && cmake --build build`).

* Extensions are precompiled to a `.dll` (or `.obj`/static archive) yourself;
  `run -e` loads the DLL with `LoadLibrary`. The toolchain does not bundle
  `.c` sources into a temporary process DLL.
* Executable discovery uses `GetModuleFileNameA`, `PATH` splits on `;`, and
  scratch files go to `%TEMP%` — no `/proc/self/exe`, `/tmp` or `:` separator.

## License

GPL-3.0.
