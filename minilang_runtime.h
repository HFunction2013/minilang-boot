/* minilang_runtime.h — public API for user C extensions.
 *
 * Link your extension into a native executable with:
 *
 *     ./minilang build -e prog.mil myext.o      # precompiled object/archive
 *     ./minilang build -e prog.mil myext.so     # precompiled shared library
 *
 * or load a prebuilt shared library at run time (C bootstrap VM only):
 *
 *     ./minilang run prog.mil myext.so
 *
 * The toolchain never compiles extension C itself: precompile it yourself
 * into an object, archive or shared library first.
 *
 * Your extension declares minilang functions with:
 *
 *     native func myfunc(args...);
 *
 * in a .mil module, and implements each as `MINI_API Value ml_native_<name>(Value ...)`
 * (base name, no module prefix — matches ml_native_lookup). This header covers
 * the whole minilang value model: nil/int/string/array/opaque.
 */
#ifndef MINILANG_RUNTIME_H
#define MINILANG_RUNTIME_H

#include <stdint.h>

/* Symbol linkage for the runtime helpers the extension calls back into.
 * When building the minilang executable/object that defines them, compile with
 * MINILANG_EXPORTS so they are dllexport'd; an extension that includes this
 * header without the macro sees them as dllimport. Non-Windows is a no-op. */
#if defined(_WIN32)
#  ifdef MINILANG_EXPORTS
#    define MINI_API __declspec(dllexport)
#  else
#    define MINI_API __declspec(dllimport)
#  endif
#else
#  define MINI_API
#endif

/* Export macro for the extension's own `ml_native_<name>` entry points. A
 * Windows /LD build only exports explicitly dllexport symbols, so annotate
 * each extension entry point with this; a no-op off Windows. */
#if defined(_WIN32)
#  define MINI_EXT_API __declspec(dllexport)
#else
#  define MINI_EXT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Value model -------------------------------------------------------
 * tag: 0=nil, 1=int, 2=string, 3=array, 4=opaque */
typedef struct {
    int64_t tag;
    int64_t payload; /* int value, or pointer cast to i64 */
} Value;

typedef struct {
    int64_t len;
    Value *data;
} Array;

/* ---- Constructors ------------------------------------------------------ */
MINI_API Value ml_nil(void);
MINI_API Value ml_mkint(int64_t x);
MINI_API Value ml_mkstr(const char *s);       /* strdup semantics */
MINI_API Value ml_mkarray(int64_t len);       /* fresh array of len nils */
MINI_API Value ml_opaque_create(void *p);     /* wrap an arbitrary C pointer */

/* ---- Type predicates --------------------------------------------------- */
MINI_API int ml_is_nil(Value v);
MINI_API int ml_is_int(Value v);
MINI_API int ml_is_str(Value v);
MINI_API int ml_is_arr(Value v);
MINI_API int ml_is_opaque(Value v);

/* ---- Accessors --------------------------------------------------------- */
MINI_API const char *ml_cstr(Value v);        /* string payload (errors if not string) */
MINI_API int64_t ml_int(Value v);             /* int payload (errors if not int) */
MINI_API void *ml_opaque_ptr(Value v);        /* unwrap opaque (errors if not opaque) */
MINI_API int64_t ml_array_len(Value v);       /* array length (errors if not array) */
MINI_API Value ml_array_get(Value arr, int64_t idx);  /* bounds-checked */
MINI_API void ml_array_set(Value arr, int64_t idx, Value val); /* bounds-checked */
MINI_API Value ml_array_create(int64_t count, Value *items);   /* wrap existing items */

/* ---- Arithmetic / comparison (mirror the VM operators) ----------------- */
MINI_API Value ml_add(Value a, Value b);  /* int add, string concat, int+str to text */
MINI_API Value ml_sub(Value a, Value b);
MINI_API Value ml_mul(Value a, Value b);
MINI_API Value ml_div(Value a, Value b);
MINI_API Value ml_mod(Value a, Value b);
MINI_API Value ml_neg(Value a);
MINI_API Value ml_eq(Value a, Value b);   /* ints/strings by value; opaque by identity */
MINI_API Value ml_neq(Value a, Value b);
MINI_API Value ml_lt(Value a, Value b);
MINI_API Value ml_gt(Value a, Value b);
MINI_API Value ml_lte(Value a, Value b);
MINI_API Value ml_gte(Value a, Value b);
MINI_API Value ml_and(Value a, Value b);  /* truthy-based, returns int 0/1 */
MINI_API Value ml_or(Value a, Value b);
MINI_API Value ml_not(Value a);           /* truthy-negation, returns int 0/1 */

/* ---- I/O / misc (mirror builtins) -------------------------------------- */
MINI_API void ml_print(Value v, int32_t newline);
MINI_API Value ml_system(Value cmd);      /* shell command -> exit code */
MINI_API Value ml_readline(void);         /* one line from stdin */

/* ---- Additional runtime helpers used by the generated C backend -------- */
MINI_API int ml_is_truthy(Value v);
MINI_API Value ml_array_make(Value sizev, Value init);
MINI_API Value ml_argc(void);
MINI_API Value ml_argv(Value idxv);
MINI_API Value ml_readfile(Value pathv);
MINI_API Value ml_fileexists(Value pathv);
MINI_API Value ml_writefile(Value pathv, Value contentv);
MINI_API Value ml_readfilebytes(Value pathv);
MINI_API Value ml_writefilebytes(Value pathv, Value arrv);
MINI_API Value ml_chr(Value nv);
MINI_API void ml_set_args(int argc, char **argv);
MINI_API Value ml_len(Value v);
MINI_API Value ml_charat(Value s, Value idx);
MINI_API Value ml_substr(Value s, Value start, Value len);
MINI_API Value ml_tostring(Value v);
MINI_API Value ml_toint(Value v);
MINI_API Value ml_strcmp(Value a, Value b);
MINI_API Value ml_readall(void);

/* ---- Built-in packed native entry points referenced by the generated C ----
 * Declared with C linkage so a C++ driver (g++) does not mangle the calls. */
MINI_API Value ml_native_sqrt(Value n);
MINI_API Value ml_native_abs(Value n);
MINI_API Value ml_native_time(void);
MINI_API Value ml_native_clock_ms(void);
MINI_API Value ml_native_getenv(Value name);
MINI_API Value ml_native_cwrite(Value s);
MINI_API Value ml_native_exit(Value code);
MINI_API Value ml_native_csystem(Value cmd);
MINI_API Value ml_native_native_invoke(Value name, Value args);
MINI_API Value ml_native_ext_build_load(Value exts, Value names, Value params);

/* ---- Native registry (C bootstrap VM) -----------------------------------
 * The VM looks natives up by base name via ml_native_lookup. When a prebuilt
 * shared library is loaded, each declared `native func name(...)` is resolved
 * as `ml_native_<name>` in the library and registered here. Not required for
 * build -e linking (the C backend references ml_native_<name> symbols
 * directly). */
typedef Value (*NativeFn)(void *vm, Value *args, int argc);
MINI_API void ml_native_register(const char *name, NativeFn fn);

#ifdef __cplusplus
}
#endif

#endif /* MINILANG_RUNTIME_H */
