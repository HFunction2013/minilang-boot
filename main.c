#include "minilang.h"
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
/* Windows (MSVC and MinGW both define _WIN32; only MSVC defines _MSC_VER,
 * so branch on _WIN32 — MinGW has no dlfcn.h and no <sys/wait.h>). */
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#if defined(_MSC_VER)
/* minilang.h already maps strdup/getcwd/access/realpath for MSVC. */
#define stat _stat
#define S_ISDIR(m) ((m) & _S_IFDIR)
#endif
#define ml_getpid() ((long)_getpid())
#else
#include <sys/wait.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dlfcn.h>
#define ml_getpid() ((long)getpid())
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#if defined(_WIN32)
#define ML_PATH_SEP '\\'
#define ML_PATH_LIST_SEP ';'
#define ML_SHLIB_EXT "dll"
#elif defined(__APPLE__)
#define ML_PATH_SEP '/'
#define ML_PATH_LIST_SEP ':'
#define ML_SHLIB_EXT "dylib"
#define ML_SHARED_FLAG "-dynamiclib -undefined dynamic_lookup"
#else
#define ML_PATH_SEP '/'
#define ML_PATH_LIST_SEP ':'
#define ML_SHLIB_EXT "so"
#define ML_SHARED_FLAG "-shared"
#endif

/* argv[0] as given to main(), used as a last resort to locate the executable. */
static char g_argv0[4096] = "";

char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open file: %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}
/* Is this path a directory? */
static int is_dir(const char *p) {
    if (!p || !p[0]) { return 0; }
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Copy dirname(path) into out, resolving it to an absolute path. */
static void abs_dirname(const char *path, char *out, size_t outsz) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", path);
    char *slash = strrchr(buf, '/');
#ifdef _WIN32
    /* Windows paths use '\' too; take whichever separator comes last. */
    char *bslash = strrchr(buf, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    if (slash) {
        *slash = '\0';
#ifdef _WIN32
        if (!buf[0]) { snprintf(buf, sizeof(buf), "\\"); }
        else if (strlen(buf) == 2 && buf[1] == ':') { buf[2] = '\\'; buf[3] = '\0'; }
#else
        if (!buf[0]) { snprintf(buf, sizeof(buf), "/"); }
#endif
    } else {
        snprintf(buf, sizeof(buf), ".");
    }
    char resolved[4096];
    if (realpath(buf, resolved)) {
        snprintf(out, outsz, "%s", resolved);
    } else {
        snprintf(out, outsz, "%s", buf);
    }
}

/**
 * Directory holding the minilang executable. Needed to find syslib/ and
 * runtime.c no matter where the user runs from.
 *
 * /proc/self/exe exists on Linux only, so fall back to the macOS dynamic-loader
 * API and finally to argv[0] (resolved through PATH when it has no slash).
 * Without this the syslib path degrades to "syslib" relative to the *current
 * working directory*, which breaks as soon as you run from anywhere else.
 */
static int get_exe_dir(char *out, size_t outsz) {
    char exe[4096];

#ifdef _WIN32
    /* Windows: ask the loader for the full image path (no /proc here). */
    {
        DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe) - 1);
        if (n > 0 && n < sizeof(exe)) {
            exe[n] = '\0';
            char *slash = strrchr(exe, '\\');
            if (!slash) slash = strrchr(exe, '/');
            if (slash && slash != exe) {
                *slash = '\0';
                snprintf(out, outsz, "%s", exe);
                return 1;
            }
        }
    }
#else
    /* Linux */
    {
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) {
            exe[n] = '\0';
            char resolved[4096];
            if (realpath(exe, resolved)) { snprintf(exe, sizeof(exe), "%s", resolved); }
            char *slash = strrchr(exe, '/');
            if (slash && slash != exe) {
                *slash = '\0';
                snprintf(out, outsz, "%s", exe);
                return 1;
            }
        }
    }
#endif
#if defined(__APPLE__)
    /* macOS: there is no /proc, so ask the dynamic loader instead */
    {
        uint32_t size = (uint32_t)sizeof(exe);
        if (_NSGetExecutablePath(exe, &size) == 0) {
            char resolved[4096];
            if (realpath(exe, resolved)) { snprintf(exe, sizeof(exe), "%s", resolved); }
            char *slash = strrchr(exe, '/');
            if (slash && slash != exe) {
                *slash = '\0';
                snprintf(out, outsz, "%s", exe);
                return 1;
            }
        }
    }
#endif
    /* Last resort: argv[0], searched through PATH when it is a bare name */
    if (g_argv0[0]) {
        if (strchr(g_argv0, '/') || strchr(g_argv0, '\\')) {
            abs_dirname(g_argv0, out, outsz);
            return 1;
        }
        const char *path_env = getenv("PATH");
        if (path_env) {
            char pathcopy[8192];
            snprintf(pathcopy, sizeof(pathcopy), "%s", path_env);
            /* Split on the platform's PATH separator (';' on Windows, ':' elsewhere)
             * without strtok_r: MSVC has no strtok_r and MinGW's availability varies. */
            const char *start = pathcopy;
            while (start && *start) {
                const char *sep = strchr(start, ML_PATH_LIST_SEP);
                char one[8192];
                size_t len = sep ? (size_t)(sep - start) : strlen(start);
                if (len >= sizeof(one)) len = sizeof(one) - 1;
                memcpy(one, start, len);
                one[len] = '\0';
                if (one[0]) {
                    char cand[8192];
                    snprintf(cand, sizeof(cand), "%s%c%s", one, ML_PATH_SEP, g_argv0);
                    /* access(..., 0) = existence check; X_OK does not exist on Windows. */
                    if (access(cand, 0) == 0) {
                        abs_dirname(cand, out, outsz);
                        return 1;
                    }
                }
                start = sep ? sep + 1 : NULL;
            }
        }
    }
    return 0;
}

/* Compute syslib directory: MINILANG_DIR/syslib if set, else <exe_dir>/syslib, else "syslib" */
static void get_syslib_dir(char *out, size_t outsz) {
    const char *dir = getenv("MINILANG_DIR");
    if (dir && dir[0]) {
        snprintf(out, outsz, "%s/syslib", dir);
        return;
    }
    char exe_dir[4096];
    if (get_exe_dir(exe_dir, sizeof(exe_dir))) {
        snprintf(out, outsz, "%s/syslib", exe_dir);
        /* Only trust it if it is really there; otherwise keep the old
           cwd-relative behaviour rather than pointing at a missing path. */
        if (is_dir(out)) { return; }
    }
    snprintf(out, outsz, "syslib");
}
/* Set search dirs from a file path: script_dir = dirname(file), syslib = MINILANG_DIR/syslib */
static void set_dirs_from_file(const char *file) {
    char script_dir[4096];
    /* Absolute, so `require sibling;` still works when the cwd is elsewhere */
    abs_dirname(file, script_dir, sizeof(script_dir));
    char syslib_dir[1024];
    get_syslib_dir(syslib_dir, sizeof(syslib_dir));
    parser_set_search_dirs(script_dir, syslib_dir);
}
void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}
static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <command> [args...]\n", prog);
    fprintf(stderr, "Commands:\n");
    fprintf(stderr, "  run <file>        Compile and run .mil, or run precompiled .milc bytecode\n");
    fprintf(stderr, "  bytecode <file>   Disassemble .mil, or read a .milc bytecode file\n");
    fprintf(stderr, "  dump-text <file>  Text bytecode for .mil, or read a .milc bytecode file\n");
    fprintf(stderr, "  c <file>          Compile to C source (.c)\n");
    fprintf(stderr, "  build [opts] <file>  Compile to native executable or .milc bytecode\n");
    fprintf(stderr, "    -b, --bytecode     write bytecode file (.milc, magic \"!milc\")\n");
    fprintf(stderr, "    -e, --executable   write native executable (C source + system cc -O2, default)\n");
    fprintf(stderr, "  repl              Start interactive REPL\n");
    fprintf(stderr, "  self-test [file]  Run self-hosting verification (compiler.mil, default cwd)\n");
    exit(1);
}
static void dump_bytecode_text(Program *prog) {
    printf("MINILANGBC\n");
    printf("%d\n", prog->const_count);
    for (int i = 0; i < prog->const_count; i++) {
        Value *c = &prog->constants[i];
        if (c->type == VAL_INT) {
            printf("0 %lld\n", (long long)c->as.integer);
        } else if (c->type == VAL_STRING) {
            printf("1 ");
            for (const char *p = c->as.string; *p; p++) {
                unsigned char ch = (unsigned char)*p;
                if (ch == '\\') printf("\\\\");
                else if (ch == '\n') printf("\\n");
                else if (ch == '\t') printf("\\t");
                else if (ch == '\r') printf("\\r");
                else putchar(ch);
            }
            printf("\n");
        } else {
            printf("0 0\n");
        }
    }
    printf("%d\n", prog->func_count);
    for (int i = 0; i < prog->func_count; i++) {
        FuncInfo *f = &prog->funcs[i];
        printf("%s %d %d %d\n", f->name, f->address, f->param_count, f->local_count);
    }
    int num_instr = prog->bc.count / 3;
    printf("%d\n", num_instr);
    for (int i = 0; i < num_instr; i++) {
        printf("%d %d %d\n", prog->bc.code[i*3], prog->bc.code[i*3+1], prog->bc.code[i*3+2]);
    }
}
static void dump_bytecode(Program *prog) {
    printf("=== Constants (%d) ===\n", prog->const_count);
    for (int i = 0; i < prog->const_count; i++) {
        printf("  [%d] ", i);
        value_print(prog->constants[i], 1);
    }
    printf("=== Functions (%d) ===\n", prog->func_count);
    for (int i = 0; i < prog->func_count; i++) {
        FuncInfo *f = &prog->funcs[i];
        printf("  [%d] %s addr=%d params=%d locals=%d\n", i, f->name, f->address, f->param_count, f->local_count);
    }
    printf("=== Bytecode (%d instructions) ===\n", prog->bc.count / 3);
    for (int i = 0; i < prog->bc.count; i += 3) {
        int op = prog->bc.code[i];
        int op1 = prog->bc.code[i+1];
        int op2 = prog->bc.code[i+2];
        const char *name = "???";
        switch (op) {
            case OP_CONST: name="CONST"; break;
            case OP_NIL: name="NIL"; break;
            case OP_ADD: name="ADD"; break;
            case OP_SUB: name="SUB"; break;
            case OP_MUL: name="MUL"; break;
            case OP_DIV: name="DIV"; break;
            case OP_MOD: name="MOD"; break;
            case OP_NEG: name="NEG"; break;
            case OP_EQ: name="EQ"; break;
            case OP_NEQ: name="NEQ"; break;
            case OP_LT: name="LT"; break;
            case OP_GT: name="GT"; break;
            case OP_LTE: name="LTE"; break;
            case OP_GTE: name="GTE"; break;
            case OP_AND: name="AND"; break;
            case OP_OR: name="OR"; break;
            case OP_NOT: name="NOT"; break;
            case OP_LOAD: name="LOAD"; break;
            case OP_STORE: name="STORE"; break;
            case OP_PRINT: name="PRINT"; break;
            case OP_PRINTLN: name="PRINTLN"; break;
            case OP_JMP: name="JMP"; break;
            case OP_JMPF: name="JMPF"; break;
            case OP_CALL: name="CALL"; break;
            case OP_RET: name="RET"; break;
            case OP_ARRAY: name="ARRAY"; break;
            case OP_INDEX: name="INDEX"; break;
            case OP_INDEX_SET: name="INDEX_SET"; break;
            case OP_POP: name="POP"; break;
            case OP_HALT: name="HALT"; break;
            case OP_CONCAT: name="CONCAT"; break;
            case OP_LEN: name="LEN"; break;
            case OP_CHARAT: name="CHARAT"; break;
            case OP_SUBSTR: name="SUBSTR"; break;
            case OP_TOSTRING: name="TOSTRING"; break;
            case OP_TOINT: name="TOINT"; break;
            case OP_STRCMP: name="STRCMP"; break;
            case OP_LOAD_GLOBAL: name="LOAD_GLOBAL"; break;
            case OP_STORE_GLOBAL: name="STORE_GLOBAL"; break;
            case OP_READALL: name="READALL"; break;
            case OP_MAKE_ARRAY: name="MAKE_ARRAY"; break;
            case OP_FILEEXISTS: name="FILEEXISTS"; break;
            case OP_WRITEFILE: name="WRITEFILE"; break;
            case OP_SYSTEM: name="SYSTEM"; break;
            case OP_ARGC: name="ARGC"; break;
            case OP_ARGV: name="ARGV"; break;
            case OP_READFILE: name="READFILE"; break;
            case OP_READLINE: name="READLINE"; break;
            case OP_READFILEBYTES: name="READFILEBYTES"; break;
            case OP_WRITEFILEBYTES: name="WRITEFILEBYTES"; break;
            case OP_CHR: name="CHR"; break;
            case OP_NATIVE: name="NATIVE"; break;
        }
        printf("  %4d: %-12s %6d %6d\n", i/3, name, op1, op2);
    }
}
/* Compile source -> Program. Calls exit(1) on error. */
static Program *compile_source(const char *src) {
    Lexer *lexer = lexer_new(src);
    lex(lexer);
    Parser *parser = parser_new(lexer);
    parse(parser);
    Program *prog = compile_to_bytecode(parser);
    parser_free(parser);
    lexer_free(lexer);
    return prog;
}
/* Compile source and run in VM. On POSIX this forks a child for REPL robustness;
   on Windows (no fork) it runs in-process. Returns child exit code. */
static int compile_and_run_src(const char *src) {
#ifdef _WIN32
    Program *prog = compile_source(src);
    VM *vm = vm_new(prog);
    (void)vm_run(vm);
    vm_free(vm);
    program_free(prog);
    return 0;
#else
    pid_t pid = fork();
    if (pid < 0) { fprintf(stderr, "fork failed\n"); return 1; }
    if (pid == 0) {
        Program *prog = compile_source(src);
        VM *vm = vm_new(prog);
        (void)vm_run(vm);
        vm_free(vm);
        program_free(prog);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}
/* Load a .milc bytecode file, or compile a .mil source. Calls exit(1) on error. */
static Program *load_or_compile(const char *file) {
    if (is_milc_file(file)) {
        Program *prog = program_read_milc(file);
        if (!prog) { fprintf(stderr, "Failed to load bytecode: %s\n", file); exit(1); }
        return prog;
    }
    char *src = read_file(file);
    set_dirs_from_file(file);
    Program *prog = compile_source(src);
    free(src);
    return prog;
}

/* Run a .mil file (compile) or .milc file (load). */
static int run_file(const char *file, int prog_argc, char **prog_argv) {
    if (is_milc_file(file)) {
        Program *prog = program_read_milc(file);
        if (!prog) { fprintf(stderr, "Failed to load bytecode: %s\n", file); return 1; }
        VM *vm = vm_new(prog);
        vm_set_args(vm, prog_argc, prog_argv);
        (void)vm_run(vm);
        vm_free(vm);
        program_free(prog);
        return 0;
    }
    char *src = read_file(file);
    /* Set search dirs: script dir = dirname(file), syslib = MINILANG_DIR/syslib */
    set_dirs_from_file(file);
    Program *prog = compile_source(src);
    VM *vm = vm_new(prog);
    vm_set_args(vm, prog_argc, prog_argv);
    (void)vm_run(vm);
    vm_free(vm);
    program_free(prog);
    free(src);
    return 0;
}

/* One `native func` declaration awaiting a prebuilt-library symbol. */
typedef struct { const char *name; int params; int solved; } ExtDecl;

/* A `.c` source is no longer accepted: extensions must be precompiled by the
 * user into a shared library ahead of time. */
static int is_c_source(const char *p) {
    size_t l = strlen(p);
    return l > 2 && strcmp(p + l - 2, ".c") == 0;
}

static void reject_c_source(const char *ext) {
    fprintf(stderr,
        "minilang no longer compiles C sources on the fly: '%s'.\n"
        "Precompile your extension into a shared library "
        "(.so/.dylib/.dll) and pass that instead.\n", ext);
    exit(1);
}

/* Strip a module prefix and map the rest to the packed-ABI symbol base name,
 * mirroring cg_ident: non [A-Za-z0-9_] characters become '_'. */
static void packed_base(const char *name, char *out, size_t sz) {
    const char *base = strrchr(name, '.');
    base = base ? base + 1 : name;
    size_t i = 0;
    for (const char *p = base; *p && i + 1 < sz; p++) {
        char c = *p;
        out[i++] = (isalnum((unsigned char)c) || c == '_') ? c : '_';
    }
    out[i] = '\0';
}

/* Load prebuilt shared libraries and register every declared `native func`.
 * Two entry-point conventions are resolved, in order:
 *   1. VM-style  `ml_vm_native_<name>(void*, Value*, int)` — registered directly.
 *   2. Packed    `ml_native_<base>(Value ...)` — wrapped in a prebuilt arity
 *      trampoline (ml_native_register_packed) for the interpreted VM.
 * The toolchain never compiles extension C itself; pass a prebuilt shared
 * library / object / archive. */
static void load_extensions_core(ExtDecl *decls, int nd, char **exts, int ext_count) {
    for (int e = 0; e < ext_count; e++) {
        if (is_c_source(exts[e])) reject_c_source(exts[e]);
    }
#ifndef _WIN32
    void **handles = calloc((size_t)(ext_count > 0 ? ext_count : 1), sizeof(void *));
    for (int e = 0; e < ext_count; e++) {
        void *h = dlopen(exts[e], RTLD_NOW | RTLD_GLOBAL);
        if (!h) {
            fprintf(stderr, "Cannot load extension %s: %s\n", exts[e], dlerror());
            exit(1);
        }
        handles[e] = h;
        for (int i = 0; i < nd; i++) {
            if (decls[i].solved) continue;
            char sym[512];
            snprintf(sym, sizeof(sym), "ml_vm_native_%s", decls[i].name);
            void *f = dlsym(h, sym);
            if (f) {
                ml_native_register(decls[i].name, (NativeFn)f);
                decls[i].solved = 1;
            }
        }
    }
    /* Packed-ABI fallback: ml_native_<base>(...) via the prebuilt bridge. */
    for (int e = 0; e < ext_count; e++) {
        for (int i = 0; i < nd; i++) {
            if (decls[i].solved) continue;
            char base[128]; packed_base(decls[i].name, base, sizeof(base));
            char sym[512];
            snprintf(sym, sizeof(sym), "ml_native_%s", base);
            void *f = dlsym(handles[e], sym);
            if (f) {
                ml_native_register_packed(decls[i].name, f, decls[i].params);
                decls[i].solved = 1;
            }
        }
    }
    free(handles);
#else
    HMODULE *handles = calloc((size_t)(ext_count > 0 ? ext_count : 1), sizeof(HMODULE));
    for (int e = 0; e < ext_count; e++) {
        HMODULE h = LoadLibraryA(exts[e]);
        if (!h) {
            fprintf(stderr, "Cannot load extension %s (error %lu)\n", exts[e], GetLastError());
            exit(1);
        }
        handles[e] = h;
        for (int i = 0; i < nd; i++) {
            if (decls[i].solved) continue;
            char sym[512];
            snprintf(sym, sizeof(sym), "ml_vm_native_%s", decls[i].name);
            FARPROC f = GetProcAddress(h, sym);
            if (f) {
                ml_native_register(decls[i].name, (NativeFn)(void *)f);
                decls[i].solved = 1;
            }
        }
    }
    for (int e = 0; e < ext_count; e++) {
        for (int i = 0; i < nd; i++) {
            if (decls[i].solved) continue;
            char base[128]; packed_base(decls[i].name, base, sizeof(base));
            char sym[512];
            snprintf(sym, sizeof(sym), "ml_native_%s", base);
            FARPROC f = GetProcAddress(handles[e], sym);
            if (f) {
                ml_native_register_packed(decls[i].name, (void *)f, decls[i].params);
                decls[i].solved = 1;
            }
        }
    }
    free(handles);
#endif
    for (int i = 0; i < nd; i++) {
        if (!decls[i].solved) {
            fprintf(stderr, "Extension for native '%s' was not resolved: export "
                    "ml_vm_native_%s or ml_native_%s from a prebuilt shared library.\n",
                    decls[i].name, decls[i].name, decls[i].name);
            exit(1);
        }
    }
}

/* Build the ExtDecl table from the program's native declarations and load. */
static void load_native_extensions(Program *prog, char **exts, int ext_count) {
    if (ext_count == 0) return;
    ExtDecl *decls = NULL;
    int nd = 0;
    for (int i = 0; i < prog->func_count; i++) {
        FuncInfo *fn = &prog->funcs[i];
        if (!fn->is_native) continue;
        decls = realloc(decls, (size_t)(nd + 1) * sizeof(ExtDecl));
        if (!decls) { fprintf(stderr, "out of memory\n"); exit(1); }
        decls[nd].name = fn->name;
        decls[nd].params = fn->param_count;
        decls[nd].solved = 0;
        nd++;
    }
    if (nd == 0) {
        /* No `native func` declarations in this program: nothing to register.
         * (The old Windows branch dereferenced decls[0] unconditionally.) */
        free(decls);
        return;
    }
    load_extensions_core(decls, nd, exts, ext_count);
    free(decls);
}

/* Entry point for the self-hosted VM (`main.mil run -e prog.mil ext.c ...`):
 * the mil side has already parsed the program's `native func` declarations, so
 * the ExtDecl table is built from names/params instead of the boot Program. */
int ml_ext_build_and_load(const char **ext_paths, int nextc,
                          const char **names, const int *params, int nd) {
    ExtDecl *decls = calloc((size_t)(nd > 0 ? nd : 1), sizeof(ExtDecl));
    char **exts = calloc((size_t)(nextc > 0 ? nextc : 1), sizeof(char *));
    for (int i = 0; i < nd; i++) {
        decls[i].name = names[i];
        decls[i].params = params[i];
        decls[i].solved = 0;
    }
    for (int i = 0; i < nextc; i++) exts[i] = (char *)ext_paths[i];
    load_extensions_core(decls, nd, exts, nextc);
    free(decls);
    free(exts);
    return 0;
}

/* run with user C extensions: compile -> register natives from the loaded
 * shared libs -> execute. */
static int run_with_extensions(const char *file, char **exts, int ext_count,
                               int prog_argc, char **prog_argv) {
    char *src = read_file(file);
    set_dirs_from_file(file);
    Program *prog = compile_source(src);
    load_native_extensions(prog, exts, ext_count);
    VM *vm = vm_new(prog);
    vm_set_args(vm, prog_argc, prog_argv);
    (void)vm_run(vm);
    vm_free(vm);
    program_free(prog);
    free(src);
    return 0;
}
/* REPL: interactive. Maintains env (func + top-level var declarations) across lines. */
static void repl(const char *syslib_dir) {
    (void)syslib_dir;
    /* Anchor the search dirs here: syslib/ lives next to the executable, and the
       script dir is the cwd, so `require` works from any working directory. */
    {
        char syslib[1024];
        char cwd[4096];
        get_syslib_dir(syslib, sizeof(syslib));
        if (!getcwd(cwd, sizeof(cwd))) { snprintf(cwd, sizeof(cwd), "."); }
        parser_set_search_dirs(cwd, syslib);
    }
    printf("minilang REPL (type 'quit' to exit)\n");
    /* env_source accumulates function definitions and top-level var declarations */
    char *env = malloc(1024);
    env[0] = '\0';
    int env_len = 0, env_cap = 1024;
    char line[8192];
    while (1) {
        printf("mil> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        /* strip newline */
        size_t ln = strlen(line);
        while (ln > 0 && (line[ln-1] == '\n' || line[ln-1] == '\r')) line[--ln] = '\0';
        if (ln == 0) continue;
        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) break;
        /* Multi-line block support: if braces unbalanced, keep reading */
        char *input = strdup(line);
        int input_len = (int)strlen(input);
        int input_cap = input_len + 1;
        int open_b = 0, close_b = 0;
        for (int i = 0; i < (int)strlen(line); i++) {
            if (line[i] == '{') open_b++;
            if (line[i] == '}') close_b++;
        }
        while (open_b > close_b) {
            printf("...> ");
            fflush(stdout);
            char more[8192];
            if (!fgets(more, sizeof(more), stdin)) break;
            size_t ml = strlen(more);
            while (ml > 0 && (more[ml-1] == '\n' || more[ml-1] == '\r')) more[--ml] = '\0';
            int extra = (int)ml;
            if (input_len + extra + 1 > input_cap) {
                input_cap = input_len + extra + 1;
                input = realloc(input, input_cap);
            }
            memcpy(input + input_len, more, extra);
            input_len += extra;
            input[input_len] = '\0';
            for (int i = 0; i < extra; i++) {
                if (more[i] == '{') open_b++;
                if (more[i] == '}') close_b++;
            }
        }
        char sd[1024]; get_syslib_dir(sd, sizeof(sd));
        parser_set_search_dirs(".", sd);
        /* Classify: leading keyword */
        char trimmed[8192];
        strcpy(trimmed, input);
        char *p = trimmed;
        while (*p == ' ' || *p == '\t') p++;
        int is_decl = 0;
        if (strncmp(p, "func ", 5) == 0) is_decl = 1;
        else if (strncmp(p, "var ", 4) == 0) is_decl = 1;
        else if (strncmp(p, "require ", 8) == 0) is_decl = 1;
        if (is_decl) {
            /* append to env */
            int need = env_len + input_len + 2;
            if (need > env_cap) {
                while (env_cap < need) env_cap *= 2;
                env = realloc(env, env_cap);
            }
            memcpy(env + env_len, input, input_len);
            env_len += input_len;
            env[env_len++] = '\n';
            env[env_len] = '\0';
            printf("(defined)\n");
            free(input);
            continue;
        }
        /* Expression vs statement: if not ending with ';' or '}', wrap as println */
        int ends_semi = (input_len > 0 && input[input_len-1] == ';');
        int ends_brace = (input_len > 0 && input[input_len-1] == '}');
        char *src = malloc(env_len + input_len + 128);
        int slen = 0;
        memcpy(src, env, env_len); slen += env_len;
        memcpy(src + slen, "func main() {", 13); slen += 13;
        if (!ends_semi && !ends_brace) {
            memcpy(src + slen, " println (", 10); slen += 10;
            memcpy(src + slen, input, input_len); slen += input_len;
            memcpy(src + slen, ");", 2); slen += 2;
        } else {
            memcpy(src + slen, input, input_len); slen += input_len;
        }
        memcpy(src + slen, " return 0; }", 12); slen += 12;
        src[slen] = '\0';
        compile_and_run_src(src);
        free(src);
        free(input);
    }
    free(env);
}
/* build -b: compile to .milc */
static int build_bytecode(const char *file) {
    char *src = read_file(file);
    set_dirs_from_file(file);
    Program *prog = compile_source(src);
    char outpath[1024];
    snprintf(outpath, sizeof(outpath), "%s", file);
    char *dot = strrchr(outpath, '.');
    if (dot) strcpy(dot, ".milc");
    else strcat(outpath, ".milc");
    if (program_write_milc(prog, outpath) != 0) {
        fprintf(stderr, "Cannot write %s\n", outpath);
        return 1;
    }
    printf("Bytecode written: %s (magic \"%s\")\n", outpath, MILC_MAGIC);
    program_free(prog);
    free(src);
    return 0;
}
/* Resolve the system C compiler: $CC override, otherwise the platform default.
 * The C backend has no LLVM dependency. */
static void cc_tool(char *out, size_t outsz) {
    const char *cc = getenv("CC");
    if (cc && cc[0]) { snprintf(out, outsz, "%s", cc); return; }
#ifdef _WIN32
    snprintf(out, outsz, "clang");
#else
    snprintf(out, outsz, "cc");
#endif
}

/* build -e [prebuilt ext libs ...]: generate a C translation unit, then compile
 * it with the system C compiler at -O2 and link against runtime.c plus any
 * user-supplied precompiled extension objects/libraries.
 *
 * Pipeline:
 *   1. generate_c_source() writes <base>.c
 *   2. `<cc> -O2 -I<runtime dir> <base>.c runtime.c [ext libs] -o <bin>`
 *
 * Extension `.c` sources are NOT compiled here: precompile them yourself into a
 * shared library / object / static archive and pass that artefact instead. */
static int build_executable(const char *file, char **exts, int ext_count, int dynamic) {
    (void)dynamic;
    char *src = read_file(file);
    set_dirs_from_file(file);
    Lexer *lexer = lexer_new(src);
    lex(lexer);
    Parser *parser = parser_new(lexer);
    parse(parser);
    char *c_src = generate_c_source(parser);
    char base[1024];
    snprintf(base, sizeof(base), "%s", file);
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    char cpath[1024], binpath[1024];
    snprintf(cpath, sizeof(cpath), "%s.c", base);
#ifdef _WIN32
    snprintf(binpath, sizeof(binpath), "%s.exe", base);
#else
    snprintf(binpath, sizeof(binpath), "%s", base);
#endif
    FILE *f = fopen(cpath, "w");
    if (!f) { fprintf(stderr, "Cannot write %s\n", cpath); exit(1); }
    fputs(c_src, f);
    fclose(f);
    free(c_src);

    /* Reject raw C extension sources: the toolchain never compiles them. */
    for (int i = 0; i < ext_count; i++) {
        if (is_c_source(exts[i])) reject_c_source(exts[i]);
    }

    char exe_dir[1024];
    {
        const char *d = getenv("MINILANG_DIR");
        if (d && d[0]) snprintf(exe_dir, sizeof(exe_dir), "%s", d);
        else if (!get_exe_dir(exe_dir, sizeof(exe_dir))) snprintf(exe_dir, sizeof(exe_dir), ".");
    }
    /* Locate runtime.c: alongside the toolchain dir first, then the sibling
     * minilang-boot repo, matching main.mil's cmd_build_executable fallback. */
    char rtc[1200];
    snprintf(rtc, sizeof(rtc), "%s/runtime.c", exe_dir);
    if (access(rtc, 0) != 0) {
        snprintf(rtc, sizeof(rtc), "%s/../minilang-boot/runtime.c", exe_dir);
        if (access(rtc, 0) != 0) {
            snprintf(rtc, sizeof(rtc), "%s/minilang-boot/runtime.c", exe_dir);
            if (access(rtc, 0) != 0) snprintf(rtc, sizeof(rtc), "../minilang-boot/runtime.c");
        }
    }
    /* Directory of runtime.c — exposed via -I so the generated source and any
     * prebuilt extension headers can find minilang_runtime.h. */
    char rtc_dir[1200];
    snprintf(rtc_dir, sizeof(rtc_dir), "%s", rtc);
    {
        char *slash = strrchr(rtc_dir, '/');
#ifdef _WIN32
        char *bs = strrchr(rtc_dir, '\\');
        if (bs && (!slash || bs > slash)) slash = bs;
#endif
        if (slash) *slash = '\0';
        else strcpy(rtc_dir, ".");
    }

    char cc[512];
    cc_tool(cc, sizeof(cc));
    char cmd_buf[6144];
    char *p = cmd_buf;
    size_t rem = sizeof(cmd_buf);
#ifdef _WIN32
    int written = snprintf(p, rem, "\"%s\" -O2 -I\"%s\" -o \"%s\" \"%s\" \"%s\"",
             cc, rtc_dir, binpath, cpath, rtc);
#else
    int written = snprintf(p, rem, "%s -O2 -I%s -no-pie -o %s %s %s",
             cc, rtc_dir, binpath, cpath, rtc);
#endif
    if (written < 0 || (size_t)written >= rem) { fprintf(stderr, "command line too long\n"); exit(1); }
    p += written; rem -= (size_t)written;

    /* Precompiled extension artefacts (shared lib / object / archive) are
     * passed straight to the link step. */
    for (int i = 0; i < ext_count; i++) {
        const char *ext = exts[i];
        char ext_arg[1200];
#ifdef _WIN32
        snprintf(ext_arg, sizeof(ext_arg), " \"%s\"", ext);
#else
        snprintf(ext_arg, sizeof(ext_arg), " %s", ext);
        size_t sl = strlen(ext);
        if (sl > 3 && strcmp(ext + sl - 3, ".so") == 0)
            snprintf(ext_arg + strlen(ext_arg), sizeof(ext_arg) - strlen(ext_arg),
                     " -Wl,-rpath,'$ORIGIN'");
#endif
        int w2 = snprintf(p, rem, "%s", ext_arg);
        if (w2 < 0 || (size_t)w2 >= rem) { fprintf(stderr, "command line too long\n"); exit(1); }
        p += w2; rem -= (size_t)w2;
    }

    fprintf(stderr, "  $ %s\n", cmd_buf);
    int ret = system(cmd_buf);
    if (ret != 0) { fprintf(stderr, "C compilation failed (set CC or install a C compiler)\n"); exit(1); }
    printf("Executable built: %s\n", binpath);
    parser_free(parser);
    lexer_free(lexer);
    free(src);
    return 0;
}
static void self_test(const char *self) {
    /* Step 1: boot compiler compiles compiler.mil -> text A */
    /* Use fork so failures don't kill the parent shell */
    printf("=== Self-hosting test ===\n");
    FILE *probe = fopen(self, "r");
    if (!probe) {
        fprintf(stderr,
                "Cannot open file: %s\n"
                "  compiler.mil lives in the minilang-compiler checkout (it was moved\n"
                "  out of this repo when the toolchain split). Run 'minilang self-test'\n"
                "  from there, or pass the path explicitly, e.g.:\n"
                "  minilang self-test ../minilang-compiler/compiler.mil\n",
                self);
        exit(1);
    }
    fclose(probe);
#ifdef _WIN32
    {
        char *src = read_file(self);
        set_dirs_from_file(self);
        Program *prog = compile_source(src);
        dump_bytecode_text(prog);
        program_free(prog);
        free(src);
        fflush(NULL);
    }
#else
    pid_t pid = fork();
    if (pid == 0) {
        char *src = read_file(self);
        set_dirs_from_file(self);
        Program *prog = compile_source(src);
        dump_bytecode_text(prog);
        program_free(prog);
        free(src);
        fflush(NULL);
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    (void)status;
#endif
    printf("(boot compile done - compare via bootstrap_test.sh)\n");
}
int main(int argc, char **argv) {
    if (argc > 0 && argv[0]) { snprintf(g_argv0, sizeof(g_argv0), "%s", argv[0]); }
    /* Expose this host toolchain binary to the self-hosted .mil so it can
     * delegate native builds back to the boot C backend. */
#ifdef _WIN32
    _putenv_s("MINILANG_BOOT", g_argv0);
#else
    setenv("MINILANG_BOOT", g_argv0, 1);
#endif
    /* Register C-implemented native methods (sqrt, abs, time, getenv, ...). */
    ml_native_register_builtins();
    if (argc < 2) usage(argv[0]);
    const char *cmd = argv[1];
    if (strcmp(cmd, "repl") == 0) {
        repl("syslib");
        return 0;
    }
    if (strcmp(cmd, "self-test") == 0) { self_test(argc > 2 ? argv[2] : "compiler.mil"); return 0; }
    if (strcmp(cmd, "run") == 0) {
        if (argc < 3) usage(argv[0]);
        if (strcmp(argv[2], "-e") == 0 || strcmp(argv[2], "--executable") == 0) {
            /* run -e prog.mil [ext.c|ext.so ...] [-- prog args...]
             * Registers each declared `native func` from the given C
             * extensions (compiled to a shared lib when given as .c) in the
             * bootstrap VM before executing. */
            if (argc < 4) usage(argv[0]);
            char **exts = NULL;
            int ext_count = 0;
            int i = 4;
            while (i < argc && strcmp(argv[i], "--") != 0) {
                exts = realloc(exts, (size_t)(ext_count + 1) * sizeof(char *));
                if (!exts) { fprintf(stderr, "out of memory\n"); exit(1); }
                exts[ext_count++] = argv[i];
                i++;
            }
            int prog_argc = 0;
            char **prog_argv = NULL;
            /* Match plain `run file ...`: prog_argv[0] is the source file. */
            int nargs = 1 + (i < argc ? argc - i - 1 : 0);
            prog_argv = calloc((size_t)(nargs + 1), sizeof(char *));
            if (!prog_argv) { fprintf(stderr, "out of memory\n"); exit(1); }
            prog_argv[prog_argc++] = (char *)argv[3];
            if (i < argc) { /* past "--" */
                for (int j = i + 1; j < argc; j++) prog_argv[prog_argc++] = argv[j];
            }
            int rc = run_with_extensions(argv[3], exts, ext_count, prog_argc, prog_argv);
            free(prog_argv);
            free(exts);
            return rc;
        }
        return run_file(argv[2], argc - 2, argv + 2);
    }
    if (strcmp(cmd, "bytecode") == 0 || strcmp(cmd, "dump-text") == 0) {
        if (argc < 3) usage(argv[0]);
        const char *file = argv[2];
        /* .milc is loaded as-is, .mil is compiled first */
        Program *prog = load_or_compile(file);
        if (strcmp(cmd, "dump-text") == 0) dump_bytecode_text(prog);
        else dump_bytecode(prog);
        program_free(prog);
        return 0;
    }
    if (strcmp(cmd, "c") == 0) {
        if (argc < 3) usage(argv[0]);
        const char *file = argv[2];
        char *src = read_file(file);
        set_dirs_from_file(file);
        Lexer *lexer = lexer_new(src);
        lex(lexer);
        Parser *parser = parser_new(lexer);
        parse(parser);
        char *csrc = generate_c_source(parser);
        char outpath[1024];
        snprintf(outpath, sizeof(outpath), "%s", file);
        char *dot = strrchr(outpath, '.');
        if (dot) strcpy(dot, ".c");
        else strcat(outpath, ".c");
        FILE *f = fopen(outpath, "w");
        if (!f) { fprintf(stderr, "Cannot write %s\n", outpath); exit(1); }
        fputs(csrc, f);
        fclose(f);
        printf("C source written to %s\n", outpath);
        free(csrc);
        parser_free(parser);
        lexer_free(lexer);
        free(src);
        return 0;
    }
    if (strcmp(cmd, "build") == 0) {
        int idx = 2;
        int mode = 0; // 0=executable(default), 1=bytecode
        int dynamic = 0;
        if (argc > 2 && (strcmp(argv[2], "-b") == 0 || strcmp(argv[2], "--bytecode") == 0)) {
            mode = 1; idx = 3;
        } else if (argc > 2 && (strcmp(argv[2], "-e") == 0 || strcmp(argv[2], "--executable") == 0)) {
            mode = 0; idx = 3;
            /* build -e --dynamic prog.mil ext.c — link extensions dynamically */
            if (argc > 3 && strcmp(argv[3], "--dynamic") == 0) { dynamic = 1; idx = 4; }
        }
        if (argc <= idx) usage(argv[0]);
        const char *file = argv[idx];
        /* Remaining arguments (after the source file) are user C extension
         * sources linked into the executable, e.g. `build -e prog.mil ext.c`. */
        char **exts = NULL;
        int ext_count = 0;
        for (int i = idx + 1; i < argc; i++) {
            exts = realloc(exts, (size_t)(ext_count + 1) * sizeof(char *));
            if (!exts) { fprintf(stderr, "out of memory\n"); exit(1); }
            exts[ext_count++] = argv[i];
        }
        int rc;
        if (mode == 1) {
            if (ext_count > 0) fprintf(stderr, "Warning: C extensions are ignored for bytecode output\n");
            rc = build_bytecode(file);
        } else {
            rc = build_executable(file, exts, ext_count, dynamic);
        }
        free(exts);
        return rc;
    }
    usage(argv[0]);
    return 1;
}
