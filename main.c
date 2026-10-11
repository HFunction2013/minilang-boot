#include "minilang.h"
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
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
    if (slash) {
        *slash = '\0';
        if (!buf[0]) { snprintf(buf, sizeof(buf), "/"); }
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
 * Directory holding the minilang executable. Needed to find syslib/, runtime.c
 * and ir_compile.py no matter where the user runs from.
 *
 * /proc/self/exe exists on Linux only, so fall back to the macOS dynamic-loader
 * API and finally to argv[0] (resolved through PATH when it has no slash).
 * Without this the syslib path degrades to "syslib" relative to the *current
 * working directory*, which breaks as soon as you run from anywhere else.
 */
static int get_exe_dir(char *out, size_t outsz) {
    char exe[4096];

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
        if (strchr(g_argv0, '/')) {
            abs_dirname(g_argv0, out, outsz);
            return 1;
        }
        const char *path_env = getenv("PATH");
        if (path_env) {
            char pathcopy[8192];
            snprintf(pathcopy, sizeof(pathcopy), "%s", path_env);
            char *save = NULL;
            for (char *p = strtok_r(pathcopy, ":", &save); p; p = strtok_r(NULL, ":", &save)) {
                char cand[8192];
                snprintf(cand, sizeof(cand), "%s/%s", p, g_argv0);
                if (access(cand, X_OK) == 0) {
                    abs_dirname(cand, out, outsz);
                    return 1;
                }
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
    fprintf(stderr, "  llvm <file>       Compile to LLVM IR (.ll)\n");
    fprintf(stderr, "  build [opts] <file>  Compile to native executable or .milc bytecode\n");
    fprintf(stderr, "    -b, --bytecode     write bytecode file (.milc, magic \"!milc\")\n");
    fprintf(stderr, "    -e, --executable   write native executable (LLVM IR + link, default)\n");
    fprintf(stderr, "  repl              Start interactive REPL\n");
    fprintf(stderr, "  self-test         Run self-hosting verification\n");
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
/* Compile source and run in VM in a forked child (for REPL robustness). Returns child exit code. */
static int compile_and_run_src(const char *src) {
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
/* build -e: compile to native executable via LLVM */
static int build_executable(const char *file) {
    char *src = read_file(file);
    set_dirs_from_file(file);
    Lexer *lexer = lexer_new(src);
    lex(lexer);
    Parser *parser = parser_new(lexer);
    parse(parser);
    char *ir = generate_llvm_ir(parser);
    char base[1024];
    snprintf(base, sizeof(base), "%s", file);
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    char llpath[1024], opath[1024], binpath[1024];
    snprintf(llpath, sizeof(llpath), "%s.ll", base);
    snprintf(opath, sizeof(opath), "%s.o", base);
    snprintf(binpath, sizeof(binpath), "%s", base);
    FILE *f = fopen(llpath, "w");
    if (!f) { fprintf(stderr, "Cannot write %s\n", llpath); exit(1); }
    fputs(ir, f);
    fclose(f);
    free(ir);
    char cmd_buf[2048];
    char exe_dir[1024];
    {
        const char *d = getenv("MINILANG_DIR");
        if (d && d[0]) {
            snprintf(exe_dir, sizeof(exe_dir), "%s", d);
        } else if (!get_exe_dir(exe_dir, sizeof(exe_dir))) {
            snprintf(exe_dir, sizeof(exe_dir), ".");
        }
    }
    snprintf(cmd_buf, sizeof(cmd_buf), "python3 %s/ir_compile.py %s %s", exe_dir, llpath, opath);
    int ret = system(cmd_buf);
    if (ret != 0) { fprintf(stderr, "IR compilation failed\n"); exit(1); }
    snprintf(cmd_buf, sizeof(cmd_buf), "gcc -o %s %s %s/runtime.c -O2 2>&1", binpath, opath, exe_dir);
    ret = system(cmd_buf);
    if (ret != 0) { fprintf(stderr, "Link failed\n"); exit(1); }
    printf("Executable built: %s\n", binpath);
    parser_free(parser);
    lexer_free(lexer);
    free(src);
    return 0;
}
static void self_test(void) {
    const char *self = "compiler.mil";
    /* Step 1: boot compiler compiles compiler.mil -> text A */
    /* Use fork so failures don't kill the parent shell */
    printf("=== Self-hosting test ===\n");
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
    printf("(boot compile done - compare via bootstrap_test.sh)\n");
}
int main(int argc, char **argv) {
    if (argc > 0 && argv[0]) { snprintf(g_argv0, sizeof(g_argv0), "%s", argv[0]); }
    /* Register C-implemented native methods (sqrt, abs, time, getenv, ...). */
    ml_native_register_builtins();
    if (argc < 2) usage(argv[0]);
    const char *cmd = argv[1];
    if (strcmp(cmd, "repl") == 0) {
        repl("syslib");
        return 0;
    }
    if (strcmp(cmd, "self-test") == 0) { self_test(); return 0; }
    if (strcmp(cmd, "run") == 0) {
        if (argc < 3) usage(argv[0]);
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
    if (strcmp(cmd, "llvm") == 0) {
        if (argc < 3) usage(argv[0]);
        const char *file = argv[2];
        char *src = read_file(file);
        set_dirs_from_file(file);
        Lexer *lexer = lexer_new(src);
        lex(lexer);
        Parser *parser = parser_new(lexer);
        parse(parser);
        char *ir = generate_llvm_ir(parser);
        char outpath[1024];
        snprintf(outpath, sizeof(outpath), "%s", file);
        char *dot = strrchr(outpath, '.');
        if (dot) strcpy(dot, ".ll");
        else strcat(outpath, ".ll");
        FILE *f = fopen(outpath, "w");
        if (!f) { fprintf(stderr, "Cannot write %s\n", outpath); exit(1); }
        fputs(ir, f);
        fclose(f);
        printf("LLVM IR written to %s\n", outpath);
        free(ir);
        parser_free(parser);
        lexer_free(lexer);
        free(src);
        return 0;
    }
    if (strcmp(cmd, "build") == 0) {
        int idx = 2;
        int mode = 0; // 0=executable(default), 1=bytecode
        if (argc > 2 && (strcmp(argv[2], "-b") == 0 || strcmp(argv[2], "--bytecode") == 0)) {
            mode = 1; idx = 3;
        } else if (argc > 2 && (strcmp(argv[2], "-e") == 0 || strcmp(argv[2], "--executable") == 0)) {
            mode = 0; idx = 3;
        }
        if (argc <= idx) usage(argv[0]);
        const char *file = argv[idx];
        if (mode == 1) return build_bytecode(file);
        return build_executable(file);
    }
    usage(argv[0]);
    return 1;
}
