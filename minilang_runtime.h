/* minilang_runtime.h — public API for user C extensions.
 *
 * Link your extension into a native executable with:
 *
 *     ./minilang build -e prog.mil myext.c
 *     (or: ./main build -e prog.mil myext.c)
 *
 * Your extension declares minilang functions with:
 *
 *     native func myfunc(args...);
 *
 * in a .mil module, and implements each as `Value ml_native_<name>(Value ...)`
 * (base name, no module prefix — matches ml_native_lookup). Opaque handles
 * (FILE*, buffers, structs...) are returned with ml_opaque_create() and
 * unwrapped with ml_opaque_ptr().
 */
#ifndef MINILANG_RUNTIME_H
#define MINILANG_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Value: tag 0=nil, 1=int, 2=string, 3=array, 4=opaque */
typedef struct {
    int64_t tag;
    int64_t payload; /* int value, or pointer cast to i64 */
} Value;

/* Wrap / unwrap an opaque C handle (TAG_OPAQUE). */
Value ml_opaque_create(void *p);
void *ml_opaque_ptr(Value v);

/* Native registry (used by the C bootstrap VM; the LLVM backend calls
 * @ml_native_<name> symbols directly). Not required for extension linking. */
typedef Value (*NativeFn)(void *vm, Value *args, int argc);
void ml_native_register(const char *name, NativeFn fn);

/* String helpers available to extensions. */
const char *ml_cstr(Value v);      /* string payload as C string */
int64_t ml_int(Value v);           /* int payload */
Value ml_mkint(int64_t x);
Value ml_mkstr(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* MINILANG_RUNTIME_H */
