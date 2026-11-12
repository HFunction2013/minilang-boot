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
 * in a .mil module, and implements each as `Value ml_native_<name>(Value ...)`
 * (base name, no module prefix — matches ml_native_lookup). This header covers
 * the whole minilang value model: nil/int/string/array/opaque.
 */
#ifndef MINILANG_RUNTIME_H
#define MINILANG_RUNTIME_H

#include <stdint.h>

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
Value ml_nil(void);
Value ml_mkint(int64_t x);
Value ml_mkstr(const char *s);       /* strdup semantics */
Value ml_mkarray(int64_t len);       /* fresh array of len nils */
Value ml_opaque_create(void *p);     /* wrap an arbitrary C pointer */

/* ---- Type predicates --------------------------------------------------- */
int ml_is_nil(Value v);
int ml_is_int(Value v);
int ml_is_str(Value v);
int ml_is_arr(Value v);
int ml_is_opaque(Value v);

/* ---- Accessors --------------------------------------------------------- */
const char *ml_cstr(Value v);        /* string payload (errors if not string) */
int64_t ml_int(Value v);             /* int payload (errors if not int) */
void *ml_opaque_ptr(Value v);        /* unwrap opaque (errors if not opaque) */
int64_t ml_array_len(Value v);       /* array length (errors if not array) */
Value ml_array_get(Value arr, int64_t idx);  /* bounds-checked */
void ml_array_set(Value arr, int64_t idx, Value val); /* bounds-checked */
Value ml_array_create(int64_t count, Value *items);   /* wrap existing items */

/* ---- Arithmetic / comparison (mirror the VM operators) ----------------- */
Value ml_add(Value a, Value b);  /* int add, string concat, int+str to text */
Value ml_sub(Value a, Value b);
Value ml_mul(Value a, Value b);
Value ml_div(Value a, Value b);
Value ml_mod(Value a, Value b);
Value ml_neg(Value a);
Value ml_eq(Value a, Value b);   /* ints/strings by value; opaque by identity */
Value ml_neq(Value a, Value b);
Value ml_lt(Value a, Value b);
Value ml_gt(Value a, Value b);
Value ml_lte(Value a, Value b);
Value ml_gte(Value a, Value b);
Value ml_and(Value a, Value b);  /* truthy-based, returns int 0/1 */
Value ml_or(Value a, Value b);

/* ---- I/O / misc (mirror builtins) -------------------------------------- */
void ml_print(Value v, int32_t newline);
Value ml_system(Value cmd);      /* shell command -> exit code */
Value ml_readline(void);         /* one line from stdin */

/* ---- Additional runtime helpers used by the generated C backend -------- */
int ml_is_truthy(Value v);
Value ml_array_make(Value sizev, Value init);
Value ml_argc(void);
Value ml_argv(Value idxv);
Value ml_readfile(Value pathv);
Value ml_fileexists(Value pathv);
Value ml_writefile(Value pathv, Value contentv);
Value ml_readfilebytes(Value pathv);
Value ml_writefilebytes(Value pathv, Value arrv);
Value ml_chr(Value nv);
void ml_set_args(int argc, char **argv);
Value ml_len(Value v);
Value ml_charat(Value s, Value idx);
Value ml_substr(Value s, Value start, Value len);
Value ml_tostring(Value v);
Value ml_toint(Value v);
Value ml_strcmp(Value a, Value b);
Value ml_readall(void);

/* ---- Native registry (C bootstrap VM) -----------------------------------
 * The VM looks natives up by base name via ml_native_lookup. When a prebuilt
 * shared library is loaded, each declared `native func name(...)` is resolved
 * as `ml_native_<name>` in the library and registered here. Not required for
 * build -e linking (the C backend references ml_native_<name> symbols
 * directly). */
typedef Value (*NativeFn)(void *vm, Value *args, int argc);
void ml_native_register(const char *name, NativeFn fn);

#ifdef __cplusplus
}
#endif

#endif /* MINILANG_RUNTIME_H */
