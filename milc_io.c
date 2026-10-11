/* milc_io.c - .milc bytecode serialization (magic: "!milc") */
#include "minilang.h"

/* Binary format:
   "!milc" (5 bytes)
   int32 const_count
   for each const: int8 type; if int: int64; if string: int32 len + bytes; if array: len + elements
   int32 func_count
   for each func: int32 namelen + name bytes, int32 address, int32 param_count, int32 local_count
   int32 global_count
   for each: int32 namelen + name bytes
   int32 bc_count (bytecode ints)
   int32 * bytecode
*/
static void wr32(FILE *f, int32_t v) { fwrite(&v, 4, 1, f); }
static int32_t rd32(FILE *f) { int32_t v; if (fread(&v, 4, 1, f) != 1) return 0; return v; }
static void wrstr(FILE *f, const char *s) {
    int32_t len = (int32_t)strlen(s);
    wr32(f, len);
    fwrite(s, 1, len, f);
}
static char *rdstr(FILE *f) {
    int32_t len = rd32(f);
    if (len < 0 || len > 1 << 24) return NULL;
    char *s = malloc(len + 1);
    if (fread(s, 1, len, f) != (size_t)len) { free(s); return NULL; }
    s[len] = '\0';
    return s;
}

int is_milc_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[5];
    int ok = (fread(magic, 1, 5, f) == 5) && (memcmp(magic, MILC_MAGIC, 5) == 0);
    fclose(f);
    return ok;
}

int program_write_milc(Program *prog, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(MILC_MAGIC, 1, 5, f);
    wr32(f, prog->const_count);
    for (int i = 0; i < prog->const_count; i++) {
        Value *c = &prog->constants[i];
        int8_t t = (int8_t)c->type;
        fwrite(&t, 1, 1, f);
        if (c->type == VAL_INT) {
            int64_t v = c->as.integer;
            fwrite(&v, 8, 1, f);
        } else if (c->type == VAL_STRING) {
            wrstr(f, c->as.string);
        } else {
            // Array / nil: store as int 0 for simplicity (shouldn't appear in const pool)
            int64_t v = 0;
            fwrite(&v, 8, 1, f);
        }
    }
    wr32(f, prog->func_count);
    for (int i = 0; i < prog->func_count; i++) {
        FuncInfo *fn = &prog->funcs[i];
        wrstr(f, fn->name);
        wr32(f, fn->address);
        wr32(f, fn->param_count);
        wr32(f, fn->local_count);
    }
    wr32(f, prog->global_count);
    for (int i = 0; i < prog->global_count; i++) {
        wrstr(f, prog->global_names[i]);
    }
    wr32(f, prog->bc.count);
    fwrite(prog->bc.code, 4, prog->bc.count, f);
    fclose(f);
    return 0;
}

Program *program_read_milc(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char magic[5];
    if (fread(magic, 1, 5, f) != 5 || memcmp(magic, MILC_MAGIC, 5) != 0) {
        fclose(f);
        return NULL;
    }
    Program *prog = program_new();
    prog->const_count = rd32(f);
    prog->const_cap = prog->const_count > 0 ? prog->const_count : 1;
    prog->constants = calloc(prog->const_cap, sizeof(Value));
    for (int i = 0; i < prog->const_count; i++) {
        int8_t t;
        if (fread(&t, 1, 1, f) != 1) { program_free(prog); fclose(f); return NULL; }
        if (t == VAL_INT) {
            int64_t v;
            if (fread(&v, 8, 1, f) != 1) { program_free(prog); fclose(f); return NULL; }
            prog->constants[i] = make_int(v);
        } else if (t == VAL_STRING) {
            char *s = rdstr(f);
            if (!s) { program_free(prog); fclose(f); return NULL; }
            prog->constants[i] = make_string(s);
            free(s);
        } else {
            int64_t v;
            if (fread(&v, 8, 1, f) != 1) { program_free(prog); fclose(f); return NULL; }
            prog->constants[i] = make_nil();
        }
    }
    prog->func_count = rd32(f);
    prog->func_cap = prog->func_count > 0 ? prog->func_count : 1;
    prog->funcs = calloc(prog->func_cap, sizeof(FuncInfo));
    for (int i = 0; i < prog->func_count; i++) {
        FuncInfo *fn = &prog->funcs[i];
        fn->name = rdstr(f);
        if (!fn->name) { program_free(prog); fclose(f); return NULL; }
        fn->address = rd32(f);
        fn->param_count = rd32(f);
        fn->local_count = rd32(f);
    }
    prog->global_count = rd32(f);
    prog->global_cap = prog->global_count > 0 ? prog->global_count : 1;
    prog->global_names = calloc(prog->global_cap, sizeof(char*));
    for (int i = 0; i < prog->global_count; i++) {
        prog->global_names[i] = rdstr(f);
        if (!prog->global_names[i]) { program_free(prog); fclose(f); return NULL; }
    }
    prog->bc.count = rd32(f);
    prog->bc.cap = prog->bc.count > 0 ? prog->bc.count : 1;
    prog->bc.code = malloc(sizeof(int) * (prog->bc.count > 0 ? prog->bc.count : 1));
    if (fread(prog->bc.code, 4, prog->bc.count, f) != (size_t)prog->bc.count) {
        program_free(prog); fclose(f); return NULL;
    }
    fclose(f);
    return prog;
}
