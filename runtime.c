/* LLVM backend runtime for minilang */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int g_argc = 0;
static char **g_argv = NULL;

void ml_set_args(int argc, char **argv) {
    g_argc = argc;
    g_argv = argv;
}

typedef struct {
    int64_t tag;     /* 0=nil, 1=int, 2=string, 3=array */
    int64_t payload; /* int value, or pointer cast to i64 */
} Value;

typedef struct {
    int64_t len;
    Value *data;
} Array;

#define TAG_NIL 0
#define TAG_INT 1
#define TAG_STR 2
#define TAG_ARR 3

static Value vnil(void) { Value v = {TAG_NIL, 0}; return v; }
static Value vint(int64_t x) { Value v = {TAG_INT, x}; return v; }
static Value vstr(const char *s) { Value v = {TAG_STR, (int64_t)(intptr_t)strdup(s ? s : "")}; return v; }

static const char *cstr(Value v) {
    if (v.tag == TAG_STR) return (const char*)(intptr_t)v.payload;
    return "";
}

static Array *carr(Value v) {
    if (v.tag == TAG_ARR) return (Array*)(intptr_t)v.payload;
    return NULL;
}

void ml_print(Value v, int32_t newline) {
    switch (v.tag) {
        case TAG_NIL: printf("nil"); break;
        case TAG_INT: printf("%lld", (long long)v.payload); break;
        case TAG_STR: printf("%s", cstr(v)); break;
        case TAG_ARR: {
            Array *a = carr(v);
            printf("[");
            for (int64_t i = 0; i < a->len; i++) {
                if (i > 0) printf(", ");
                ml_print(a->data[i], 0);
            }
            printf("]");
            break;
        }
    }
    if (newline) printf("\n");
    fflush(stdout);
}

static int truthy(Value v) {
    switch (v.tag) {
        case TAG_NIL: return 0;
        case TAG_INT: return v.payload != 0;
        case TAG_STR: return strlen(cstr(v)) > 0;
        case TAG_ARR: return 1;
    }
    return 0;
}

Value ml_add(Value a, Value b) {
    if (a.tag == TAG_INT && b.tag == TAG_INT) return vint(a.payload + b.payload);
    if (a.tag == TAG_STR || b.tag == TAG_STR) {
        char buf_a[64], buf_b[64];
        const char *sa, *sb;
        if (a.tag == TAG_STR) sa = cstr(a);
        else { snprintf(buf_a, sizeof(buf_a), "%lld", (long long)a.payload); sa = buf_a; }
        if (b.tag == TAG_STR) sb = cstr(b);
        else { snprintf(buf_b, sizeof(buf_b), "%lld", (long long)b.payload); sb = buf_b; }
        size_t la = strlen(sa), lb = strlen(sb);
        char *r = malloc(la + lb + 1);
        memcpy(r, sa, la); memcpy(r + la, sb, lb);
        r[la + lb] = '\0';
        Value v = {TAG_STR, (int64_t)(intptr_t)r};
        return v;
    }
    fprintf(stderr, "type error: +\n"); exit(1);
}

Value ml_sub(Value a, Value b) {
    if (a.tag == TAG_INT && b.tag == TAG_INT) return vint(a.payload - b.payload);
    fprintf(stderr, "type error: -\n"); exit(1);
}
Value ml_mul(Value a, Value b) {
    if (a.tag == TAG_INT && b.tag == TAG_INT) return vint(a.payload * b.payload);
    fprintf(stderr, "type error: *\n"); exit(1);
}
Value ml_div(Value a, Value b) {
    if (a.tag == TAG_INT && b.tag == TAG_INT) {
        if (b.payload == 0) { fprintf(stderr, "division by zero\n"); exit(1); }
        return vint(a.payload / b.payload);
    }
    fprintf(stderr, "type error: /\n"); exit(1);
}
Value ml_mod(Value a, Value b) {
    if (a.tag == TAG_INT && b.tag == TAG_INT) {
        if (b.payload == 0) { fprintf(stderr, "modulo by zero\n"); exit(1); }
        return vint(a.payload % b.payload);
    }
    fprintf(stderr, "type error: %%\n"); exit(1);
}
Value ml_neg(Value a) {
    if (a.tag == TAG_INT) return vint(-a.payload);
    fprintf(stderr, "type error: unary -\n"); exit(1);
}

static int cmp(Value a, Value b) {
    if (a.tag == TAG_INT && b.tag == TAG_INT) {
        if (a.payload < b.payload) return -1;
        if (a.payload > b.payload) return 1;
        return 0;
    }
    if (a.tag == TAG_STR && b.tag == TAG_STR) return strcmp(cstr(a), cstr(b));
    if (a.tag == TAG_NIL && b.tag == TAG_NIL) return 0;
    return -2;
}

Value ml_eq(Value a, Value b) { return vint(cmp(a, b) == 0); }
Value ml_neq(Value a, Value b) { return vint(cmp(a, b) != 0); }
Value ml_lt(Value a, Value b) { return vint(cmp(a, b) < 0); }
Value ml_gt(Value a, Value b) { return vint(cmp(a, b) > 0); }
Value ml_lte(Value a, Value b) { return vint(cmp(a, b) <= 0); }
Value ml_gte(Value a, Value b) { return vint(cmp(a, b) >= 0); }
Value ml_and(Value a, Value b) { return vint(truthy(a) && truthy(b)); }
Value ml_or(Value a, Value b) { return vint(truthy(a) || truthy(b)); }
Value ml_not(Value a) { return vint(!truthy(a)); }

Value ml_array_create(int64_t count, Value *items) {
    Array *a = malloc(sizeof(Array));
    a->len = count;
    a->data = malloc(sizeof(Value) * (count > 0 ? count : 1));
    for (int64_t i = 0; i < count; i++) a->data[i] = items[i];
    Value v = {TAG_ARR, (int64_t)(intptr_t)a};
    return v;
}

/* array(size, init): create array of given size filled with init */
Value ml_array_make(Value sizev, Value init) {
    int64_t size = sizev.payload;
    if (size < 0) size = 0;
    Array *a = malloc(sizeof(Array));
    a->len = size;
    a->data = malloc(sizeof(Value) * (size > 0 ? size : 1));
    for (int64_t i = 0; i < size; i++) a->data[i] = init;
    Value v = {TAG_ARR, (int64_t)(intptr_t)a};
    return v;
}

Value ml_argc(void) {
    Value v = {TAG_INT, g_argc};
    return v;
}

Value ml_argv(Value idxv) {
    int idx = (int)idxv.payload;
    if (idx < 0 || idx >= g_argc) {
        Value v = {TAG_STR, (int64_t)(intptr_t)strdup("")};
        return v;
    }
    Value v = {TAG_STR, (int64_t)(intptr_t)strdup(g_argv[idx])};
    return v;
}

Value ml_readfile(Value pathv) {
    const char *path = (const char*)(intptr_t)pathv.payload;
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open file: %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    Value v = {TAG_STR, (int64_t)(intptr_t)buf};
    return v;
}
Value ml_fileexists(Value pathv) {
    const char *path = (const char*)(intptr_t)pathv.payload;
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return vint(1); }
    return vint(0);
}

/* readFileBytes(path): read a file as raw bytes -> array of ints (0..255) */
Value ml_readfilebytes(Value pathv) {
    const char *path = (const char*)(intptr_t)pathv.payload;
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open file: %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) size = 0;
    Value *items = malloc(sizeof(Value) * (size > 0 ? size : 1));
    long n = 0;
    int c;
    while (n < size && (c = fgetc(f)) != EOF) items[n++] = vint((unsigned char)c);
    fclose(f);
    Value v = ml_array_create(n, items);
    free(items);
    return v;
}

/* chr(n): single-character string from byte value */
Value ml_chr(Value nv) {
    char buf[2];
    buf[0] = (char)(unsigned char)(nv.payload & 0xFF);
    buf[1] = '\0';
    return vstr(buf);
}

/* writeFileBytes(path, arr): write int array as raw bytes -> 1 on success */
Value ml_writefilebytes(Value pathv, Value arrv) {
    const char *path = (const char*)(intptr_t)pathv.payload;
    FILE *f = fopen(path, "wb");
    if (!f) return vint(0);
    int ok = 1;
    Array *a = carr(arrv);
    if (a) {
        for (int64_t i = 0; i < a->len; i++) {
            unsigned char b = 0;
            if (a->data[i].tag == TAG_INT) b = (unsigned char)(a->data[i].payload & 0xFF);
            else if (a->data[i].tag == TAG_STR) {
                const char *s = cstr(a->data[i]);
                if (s && s[0]) b = (unsigned char)s[0];
            }
            if (fwrite(&b, 1, 1, f) != 1) { ok = 0; break; }
        }
    }
    fclose(f);
    return vint(ok);
}

Value ml_writefile(Value pathv, Value contentv) {
    const char *path = (const char*)(intptr_t)pathv.payload;
    const char *content = (const char*)(intptr_t)contentv.payload;
    FILE *f = fopen(path, "w");
    if (f) {
        fwrite(content, 1, strlen(content), f);
        fclose(f);
        return vint(1);
    }
    return vint(0);
}

Value ml_system(Value cmdv) {
    const char *cmd = (const char*)(intptr_t)cmdv.payload;
    int rc = system(cmd);
    return vint(rc);
}

Value ml_readline(void) {
    char buf[8192];
    if (fgets(buf, sizeof(buf), stdin) == NULL) {
        Value e = {TAG_STR, (int64_t)(intptr_t)strdup("")};
        return e;
    }
    size_t n = strlen(buf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
    char *s = strdup(buf);
    Value v = {TAG_STR, (int64_t)(intptr_t)s};
    return v;
}

Value ml_array_get(Value arr, int64_t idx) {
    if (arr.tag == TAG_ARR) {
        Array *a = carr(arr);
        if (idx < 0 || idx >= a->len) { fprintf(stderr, "array index %lld out of bounds\n", (long long)idx); exit(1); }
        return a->data[idx];
    }
    if (arr.tag == TAG_STR) {
        const char *s = cstr(arr);
        int64_t len = strlen(s);
        if (idx < 0 || idx >= len) return vint(0);
        return vint((unsigned char)s[idx]);
    }
    fprintf(stderr, "cannot index non-array\n"); exit(1);
}

void ml_array_set(Value arr, int64_t idx, Value val) {
    if (arr.tag == TAG_ARR) {
        Array *a = carr(arr);
        if (idx < 0 || idx >= a->len) { fprintf(stderr, "array index %lld out of bounds\n", (long long)idx); exit(1); }
        a->data[idx] = val;
        return;
    }
    fprintf(stderr, "cannot index-assign non-array\n"); exit(1);
}

Value ml_len(Value v) {
    if (v.tag == TAG_STR) return vint(strlen(cstr(v)));
    if (v.tag == TAG_ARR) return vint(carr(v)->len);
    fprintf(stderr, "len expects string or array\n"); exit(1);
}

Value ml_charat(Value s, Value idx) {
    if (s.tag != TAG_STR || idx.tag != TAG_INT) { fprintf(stderr, "charAt expects string, int\n"); exit(1); }
    const char *str = cstr(s);
    int64_t i = idx.payload;
    int64_t len = strlen(str);
    if (i < 0 || i >= len) return vint(0);
    return vint((unsigned char)str[i]);
}

Value ml_substr(Value s, Value start, Value len) {
    if (s.tag != TAG_STR || start.tag != TAG_INT || len.tag != TAG_INT) {
        fprintf(stderr, "substr expects string, int, int\n"); exit(1);
    }
    const char *str = cstr(s);
    int64_t slen = strlen(str);
    int64_t st = start.payload, ln = len.payload;
    if (st < 0) st = 0;
    if (st > slen) st = slen;
    if (st + ln > slen) ln = slen - st;
    if (ln < 0) ln = 0;
    char *r = malloc(ln + 1);
    memcpy(r, str + st, ln);
    r[ln] = '\0';
    Value v = {TAG_STR, (int64_t)(intptr_t)r};
    return v;
}

Value ml_tostring(Value v) {
    if (v.tag == TAG_STR) return vstr(cstr(v));
    if (v.tag == TAG_INT) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)v.payload);
        return vstr(buf);
    }
    if (v.tag == TAG_NIL) return vstr("nil");
    return vstr("<array>");
}

Value ml_toint(Value v) {
    if (v.tag == TAG_INT) return v;
    if (v.tag == TAG_STR) return vint(atoll(cstr(v)));
    fprintf(stderr, "toInt expects int or string\n"); exit(1);
}

Value ml_strcmp(Value a, Value b) {
    if (a.tag == TAG_STR && b.tag == TAG_STR) return vint(strcmp(cstr(a), cstr(b)));
    fprintf(stderr, "strcmp expects two strings\n"); exit(1);
}

Value ml_readall(void) {
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    int c;
    while ((c = fgetc(stdin)) != EOF) {
        if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    Value v = {TAG_STR, (int64_t)(intptr_t)buf};
    return v;
}
