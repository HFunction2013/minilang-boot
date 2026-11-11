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
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

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
#ifdef _WIN32
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return make_int((int64_t)(count.QuadPart * 1000 / freq.QuadPart));
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return make_int((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
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

/* native_invoke(name, boxed_args) -> mil-boxed result.
 *
 * Generic dispatch used by the self-hosted VM (vm.mil): when it hits
 * OP_NATIVE for a name it does not hard-code, it forwards the call here.
 * boxed_args is a mil array of mil values, each boxed as [type_tag, payload];
 * we unwrap them into host Values, look the name up in the registry (so
 * extensions loaded through `run -e` are reachable) and wrap the result back. */
static Value n_native_invoke(VM *vm, Value *args, int argc) {
    (void)argc;
    const char *name = args[0].as.string;
    Array *boxes = &args[1].as.array;
    NativeFn fn = ml_native_lookup(name);
    if (!fn) {
        fprintf(stderr, "VM error: native function '%s' is not registered\n", name);
        exit(1);
    }
    Value unpacked[256];
    int n = boxes->count < 256 ? boxes->count : 256;
    for (int i = 0; i < n; i++) {
        Value *box = &boxes->items[i];
        if (box->type != VAL_ARRAY || box->as.array.count < 2) {
            unpacked[i] = *box;
            continue;
        }
        int64_t tag = box->as.array.items[0].as.integer;
        if (tag == VAL_NIL) unpacked[i] = make_nil();
        else if (tag == VAL_OPAQUE) {
            /* mil boxed opaque payload is an int holding the pointer */
            Value o = make_int(box->as.array.items[1].as.integer);
            o.type = VAL_OPAQUE;
            unpacked[i] = o;
        } else unpacked[i] = box->as.array.items[1];
    }
    Value r = fn(vm, unpacked, n);
    /* Re-box as [type_tag, payload], matching v_int/v_str/v_nil/v_arr. Opaque
     * results are boxed with an int payload so the mil VM can test truthiness
     * and compare identity. */
    Value cells[2];
    cells[0] = make_int((int64_t)r.type);
    if (r.type == VAL_NIL) cells[1] = make_int(0);
    else if (r.type == VAL_OPAQUE) cells[1] = make_int(r.as.integer);
    else cells[1] = r;
    return make_array(cells, 2);
}

/* ext_build_load(ext_paths, names, params) -> int.
 *
 * Compile + dlopen user C extensions on behalf of the self-hosted VM
 * (main.mil run -e). args[0] = string array of .c/.so paths,
 * args[1] = string array of declared native names,
 * args[2] = int array of parameter counts. */
static Value n_ext_build_load(VM *vm, Value *args, int argc) {
    (void)vm; (void)argc;
    Array *exts = &args[0].as.array;
    Array *names = &args[1].as.array;
    Array *params = &args[2].as.array;
    const char **ep = malloc(sizeof(char *) * (size_t)(exts->count > 0 ? exts->count : 1));
    const char **np = malloc(sizeof(char *) * (size_t)(names->count > 0 ? names->count : 1));
    int *pp = malloc(sizeof(int) * (size_t)(names->count > 0 ? names->count : 1));
    for (int i = 0; i < exts->count; i++) ep[i] = exts->items[i].as.string;
    for (int i = 0; i < names->count; i++) {
        np[i] = names->items[i].as.string;
        pp[i] = (int)params->items[i].as.integer;
    }
    int rc = ml_ext_build_and_load(ep, exts->count, np, pp, names->count);
    free(ep);
    free(np);
    free(pp);
    return make_int(rc);
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
    ml_native_register("native_invoke", n_native_invoke);
    ml_native_register("ext_build_load", n_ext_build_load);
}
