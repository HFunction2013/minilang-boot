/*
 * native.c — registry of C-implemented ("native") methods callable from minilang.
 *
 * A minilang program declares a native method with:
 *
 *     native func sqrt(n);
 *
 * and calls it like any other function. The bytecode compiler emits OP_NATIVE,
 * and the VM looks the function up here by name and invokes the C pointer.
 *
 * Native C functions receive the VM (for vm->argc / vm->argv and future use),
 * an array of already-popped argument Values (args[0] is the first argument)
 * and the argument count. They return a Value that is pushed back onto the
 * minilang stack.
 *
 * New native methods are added here (or registered at runtime via
 * ml_native_register) without touching the lexer/parser/vm dispatch.
 */
#include "minilang.h"
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* ===================== Registry ===================== */
typedef struct {
    char *name;
    NativeFn fn;
} NativeEntry;

static NativeEntry *g_natives = NULL;
static int g_native_count = 0;
static int g_native_cap = 0;

void ml_native_register(const char *name, NativeFn fn) {
    for (int i = 0; i < g_native_count; i++) {
        if (strcmp(g_natives[i].name, name) == 0) {
            g_natives[i].fn = fn;
            return;
        }
    }
    if (g_native_count >= g_native_cap) {
        g_native_cap = g_native_cap ? g_native_cap * 2 : 16;
        g_natives = realloc(g_natives, sizeof(NativeEntry) * g_native_cap);
    }
    g_natives[g_native_count].name = strdup(name);
    g_natives[g_native_count].fn = fn;
    g_native_count++;
}

NativeFn ml_native_lookup(const char *name) {
    for (int i = 0; i < g_native_count; i++) {
        if (strcmp(g_natives[i].name, name) == 0) return g_natives[i].fn;
    }
    /* Module-qualified names (e.g. "math.sqrt") fall back to the bare name,
       so a native declared/registered once is callable through any module. */
    const char *dot = strrchr(name, '.');
    if (dot) {
        const char *base = dot + 1;
        for (int i = 0; i < g_native_count; i++) {
            if (strcmp(g_natives[i].name, base) == 0) return g_natives[i].fn;
        }
    }
    return NULL;
}

/* ===================== Built-in native methods ===================== */

/* sqrt(n) -> floor(sqrt(n)) as an int. Moved out of syslib/math.mil so the
   milc image no longer carries the Newton-iteration bytecode. */
static Value n_sqrt(VM *vm, Value *args, int argc) {
    (void)vm; (void)argc;
    int64_t x = args[0].as.integer;
    if (x < 0) x = 0;
    int64_t r = 0, b = 1;
    /* Find highest bit */
    while (b <= x / 2) b <<= 1;
    /* Integer square root (base-2 digits) */
    while (b > 0) {
        if ((r + b) * (r + b) <= x) r += b;
        b >>= 1;
    }
    return make_int(r);
}

/* abs(n) -> |n| */
static Value n_abs(VM *vm, Value *args, int argc) {
    (void)vm; (void)argc;
    int64_t x = args[0].as.integer;
    return make_int(x < 0 ? -x : x);
}

/* time() -> seconds since the Unix epoch */
static Value n_time(VM *vm, Value *args, int argc) {
    (void)vm; (void)args; (void)argc;
    return make_int((int64_t)time(NULL));
}

/* clock_ms() -> milliseconds of CPU/wall time since process start */
static Value n_clock_ms(VM *vm, Value *args, int argc) {
    (void)vm; (void)args; (void)argc;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return make_int((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* getenv(name) -> string value of an environment variable ("" if unset) */
static Value n_getenv(VM *vm, Value *args, int argc) {
    (void)vm; (void)argc;
    const char *v = getenv(args[0].as.string);
    return make_string(v ? v : "");
}

/* cwrite(s) -> write a string directly to stdout, return 1 */
static Value n_cwrite(VM *vm, Value *args, int argc) {
    (void)vm; (void)argc;
    fputs(args[0].as.string, stdout);
    fflush(stdout);
    return make_int(1);
}

/* exit(code) -> terminate the process with the given int exit code */
static Value n_exit(VM *vm, Value *args, int argc) {
    (void)vm; (void)argc;
    exit((int)args[0].as.integer);
    return make_nil(); /* unreachable */
}

/* system(cmd) is already a builtin opcode; provide a native alias too. */
static Value n_system(VM *vm, Value *args, int argc) {
    (void)vm; (void)argc;
    return make_int(system(args[0].as.string));
}

void ml_native_register_builtins(void) {
    ml_native_register("sqrt", n_sqrt);
    ml_native_register("abs", n_abs);
    ml_native_register("time", n_time);
    ml_native_register("clock_ms", n_clock_ms);
    ml_native_register("getenv", n_getenv);
    ml_native_register("cwrite", n_cwrite);
    ml_native_register("exit", n_exit);
    ml_native_register("csystem", n_system);
}
