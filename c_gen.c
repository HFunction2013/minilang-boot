#include "minilang.h"

/* ===================== C source generator =====================
 *
 * Emits a plain C translation unit that mirrors the minilang program. All
 * value semantics are delegated to the `ml_*` helpers in runtime.c (the packed
 * 16-byte Value model declared in minilang_runtime.h); this backend only
 * lays out locals, control flow and calls. The generated file is compiled by
 * the system C compiler with -O2 and linked against runtime.c plus any
 * user-supplied precompiled extension libraries.
 */

typedef struct {
    char *buf;
    int len;
    int cap;
    int temp_counter;
    Parser *parser;
    /* Per-function locals: source name -> C slot name. */
    char **var_names;
    char **var_slots;
    int var_count;
    int var_cap;
    /* Top-level globals: source name -> C slot name. */
    char **global_names;
    char **global_slots;
    int global_count;
    int global_cap;
    /* String constant pool. */
    char *strpool;
    int strpool_len;
    int strpool_cap;
    int str_counter;
} CGen;

static void cg_buf_init(CGen *g) {
    g->buf = malloc(4096);
    g->buf[0] = '\0';
    g->len = 0; g->cap = 4096;
    g->temp_counter = 0;
    g->var_names = NULL; g->var_slots = NULL; g->var_count = 0; g->var_cap = 0;
    g->global_names = NULL; g->global_slots = NULL; g->global_count = 0; g->global_cap = 0;
    g->strpool = malloc(4096);
    g->strpool[0] = '\0';
    g->strpool_len = 0; g->strpool_cap = 4096;
    g->str_counter = 0;
}

static void cg_reserve(CGen *g, int extra) {
    if (g->len + extra + 1 > g->cap) {
        while (g->len + extra + 1 > g->cap) g->cap *= 2;
        g->buf = realloc(g->buf, g->cap);
    }
}

static void cg_emit(CGen *g, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    cg_reserve(g, needed);
    va_start(ap, fmt);
    vsnprintf(g->buf + g->len, needed + 1, fmt, ap);
    va_end(ap);
    g->len += needed;
}

static void cg_emit_pool(CGen *g, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (g->strpool_len + needed + 1 > g->strpool_cap) {
        while (g->strpool_len + needed + 1 > g->strpool_cap) g->strpool_cap *= 2;
        g->strpool = realloc(g->strpool, g->strpool_cap);
    }
    va_start(ap, fmt);
    vsnprintf(g->strpool + g->strpool_len, needed + 1, fmt, ap);
    va_end(ap);
    g->strpool_len += needed;
}

static char *new_temp(CGen *g) {
    static char name[32];
    snprintf(name, sizeof(name), "t%d", g->temp_counter++);
    return strdup(name);
}

/* Map a minilang identifier to a legal C identifier: every character outside
 * [A-Za-z0-9_] becomes '_'. */
static void cg_ident(const char *name, char *out, size_t sz) {
    size_t i = 0;
    for (const char *p = name; *p && i + 1 < sz; p++) {
        char c = *p;
        out[i++] = (isalnum((unsigned char)c) || c == '_') ? c : '_';
    }
    out[i] = '\0';
}

static int find_var(CGen *g, const char *name) {
    for (int i = 0; i < g->var_count; i++)
        if (strcmp(g->var_names[i], name) == 0) return i;
    return -1;
}

static int find_global(CGen *g, const char *name) {
    for (int i = 0; i < g->global_count; i++)
        if (strcmp(g->global_names[i], name) == 0) return i;
    return -1;
}

static int add_var(CGen *g, const char *name) {
    int existing = find_var(g, name);
    if (existing >= 0) return existing;
    if (g->var_count >= g->var_cap) {
        g->var_cap = g->var_cap ? g->var_cap * 2 : 16;
        g->var_names = realloc(g->var_names, sizeof(char *) * g->var_cap);
        g->var_slots = realloc(g->var_slots, sizeof(char *) * g->var_cap);
    }
    g->var_names[g->var_count] = strdup(name);
    char slot[32];
    snprintf(slot, sizeof(slot), "v%d", g->var_count);
    g->var_slots[g->var_count] = strdup(slot);
    return g->var_count++;
}

static char *compile_expr(CGen *g, Node *n);

/* Pre-scan a function body to register every local `var` declaration so all
 * slots can be declared at the top of the generated C function (C99 mixed
 * declarations are allowed, but hoisting avoids redeclaration issues in
 * nested blocks). */
static void collect_vars(CGen *g, Node *n) {
    if (!n) return;
    switch (n->type) {
        case NODE_VAR_DECL:
            add_var(g, n->var_decl.name);
            collect_vars(g, n->var_decl.value);
            break;
        case NODE_ASSIGN:
            collect_vars(g, n->assign.value);
            break;
        case NODE_INDEX_ASSIGN:
            collect_vars(g, n->index_assign.target);
            collect_vars(g, n->index_assign.index);
            collect_vars(g, n->index_assign.value);
            break;
        case NODE_IF:
            collect_vars(g, n->if_stmt.cond);
            collect_vars(g, n->if_stmt.then_branch);
            collect_vars(g, n->if_stmt.else_branch);
            break;
        case NODE_WHILE:
            collect_vars(g, n->while_stmt.cond);
            collect_vars(g, n->while_stmt.body);
            break;
        case NODE_BLOCK:
            for (int i = 0; i < n->block.count; i++) collect_vars(g, n->block.stmts[i]);
            break;
        default:
            break;
    }
}

static void compile_stmt(CGen *g, Node *n) {
    if (!n) return;
    switch (n->type) {
        case NODE_VAR_DECL: {
            int idx = find_var(g, n->var_decl.name);
            char *val = compile_expr(g, n->var_decl.value);
            cg_emit(g, "    %s = %s;\n", g->var_slots[idx], val);
            free(val);
            break;
        }
        case NODE_ASSIGN: {
            int idx = find_var(g, n->assign.name);
            if (idx >= 0) {
                char *val = compile_expr(g, n->assign.value);
                cg_emit(g, "    %s = %s;\n", g->var_slots[idx], val);
                free(val);
                break;
            }
            int gidx = find_global(g, n->assign.name);
            if (gidx >= 0) {
                char *val = compile_expr(g, n->assign.value);
                cg_emit(g, "    %s = %s;\n", g->global_slots[gidx], val);
                free(val);
                break;
            }
            fprintf(stderr, "C gen: undefined var '%s'\n", n->assign.name); exit(1);
        }
        case NODE_INDEX_ASSIGN: {
            char *arr = compile_expr(g, n->index_assign.target);
            char *idx = compile_expr(g, n->index_assign.index);
            char *val = compile_expr(g, n->index_assign.value);
            cg_emit(g, "    ml_array_set(%s, %s.payload, %s);\n", arr, idx, val);
            free(arr); free(idx); free(val);
            break;
        }
        case NODE_IF: {
            char *cond = compile_expr(g, n->if_stmt.cond);
            if (n->if_stmt.else_branch) {
                cg_emit(g, "    if (ml_is_truthy(%s)) {\n", cond);
                compile_stmt(g, n->if_stmt.then_branch);
                cg_emit(g, "    } else {\n");
                compile_stmt(g, n->if_stmt.else_branch);
                cg_emit(g, "    }\n");
            } else {
                cg_emit(g, "    if (ml_is_truthy(%s)) {\n", cond);
                compile_stmt(g, n->if_stmt.then_branch);
                cg_emit(g, "    }\n");
            }
            free(cond);
            break;
        }
        case NODE_WHILE: {
            /* Re-evaluate the condition on every iteration: hoist it inside the
             * loop so the temp is refreshed each pass. */
            cg_emit(g, "    while (1) {\n");
            char *cond = compile_expr(g, n->while_stmt.cond);
            cg_emit(g, "        if (!ml_is_truthy(%s)) break;\n", cond);
            free(cond);
            compile_stmt(g, n->while_stmt.body);
            cg_emit(g, "    }\n");
            break;
        }
        case NODE_RETURN: {
            if (n->return_stmt.value) {
                char *val = compile_expr(g, n->return_stmt.value);
                cg_emit(g, "    return %s;\n", val);
                free(val);
            } else {
                cg_emit(g, "    return ml_nil();\n");
            }
            break;
        }
        case NODE_BREAK:
            cg_emit(g, "    break;\n");
            break;
        case NODE_PRINT: {
            char *val = compile_expr(g, n->print.value);
            cg_emit(g, "    ml_print(%s, %d);\n", val, n->print.newline ? 1 : 0);
            free(val);
            break;
        }
        case NODE_BLOCK:
            for (int i = 0; i < n->block.count; i++) compile_stmt(g, n->block.stmts[i]);
            break;
        case NODE_EXPR_STMT: {
            char *val = compile_expr(g, n->expr_stmt.expr);
            cg_emit(g, "    (void)(%s);\n", val);
            free(val);
            break;
        }
        default:
            fprintf(stderr, "C gen: unexpected stmt node %d\n", n->type);
            exit(1);
    }
}

/* Escape a minilang string into a C string literal body (without quotes). */
static char *c_escape(const char *src) {
    char *out = malloc(strlen(src) * 4 + 1);
    int oi = 0;
    for (int i = 0; src[i]; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"') { out[oi++] = '\\'; out[oi++] = '"'; }
        else if (c == '\\') { out[oi++] = '\\'; out[oi++] = '\\'; }
        else if (c == '\n') { out[oi++] = '\\'; out[oi++] = 'n'; }
        else if (c == '\t') { out[oi++] = '\\'; out[oi++] = 't'; }
        else if (c == '\r') { out[oi++] = '\\'; out[oi++] = 'r'; }
        else if (c < 32 || c >= 127) { oi += snprintf(out + oi, 8, "\\x%02X", c); }
        else out[oi++] = (char)c;
    }
    out[oi] = '\0';
    return out;
}

static char *compile_expr(CGen *g, Node *n) {
    switch (n->type) {
        case NODE_INT: {
            char *r = new_temp(g);
            cg_emit(g, "    Value %s = ml_mkint(%lld);\n", r, (long long)n->integer.value);
            return r;
        }
        case NODE_STRING: {
            char *escaped = c_escape(n->string.value);
            char kname[32];
            snprintf(kname, sizeof(kname), "k%d", g->str_counter++);
            cg_emit_pool(g, "static const char *%s = \"%s\";\n", kname, escaped);
            free(escaped);
            char *r = new_temp(g);
            cg_emit(g, "    Value %s = ml_mkstr(%s);\n", r, kname);
            return r;
        }
        case NODE_ARRAY_LIT: {
            int count = n->array_lit.count;
            char **elems = malloc(sizeof(char *) * (count > 0 ? count : 1));
            for (int i = 0; i < count; i++) elems[i] = compile_expr(g, n->array_lit.items[i]);
            char *r = new_temp(g);
            char earr[32];
            snprintf(earr, sizeof(earr), "e%d", g->temp_counter++);
            if (count == 0) {
                cg_emit(g, "    Value %s[1] = { ml_nil() };\n", earr);
                cg_emit(g, "    Value %s = ml_array_create(0, %s);\n", r, earr);
            } else {
                cg_emit(g, "    Value %s[%d] = { ", earr, count);
                for (int i = 0; i < count; i++) {
                    if (i > 0) cg_emit(g, ", ");
                    cg_emit(g, "%s", elems[i]);
                }
                cg_emit(g, " };\n");
                cg_emit(g, "    Value %s = ml_array_create(%d, %s);\n", r, count, earr);
            }
            for (int i = 0; i < count; i++) free(elems[i]);
            free(elems);
            return r;
        }
        case NODE_VAR: {
            int idx = find_var(g, n->var.name);
            if (idx >= 0) return strdup(g->var_slots[idx]);
            int gidx = find_global(g, n->var.name);
            if (gidx >= 0) return strdup(g->global_slots[gidx]);
            fprintf(stderr, "C gen: undefined var '%s'\n", n->var.name); exit(1);
        }
        case NODE_BINARY: {
            char *left = compile_expr(g, n->binary.left);
            char *right = compile_expr(g, n->binary.right);
            const char *func = NULL;
            switch (n->binary.op) {
                case TOK_PLUS: func = "ml_add"; break;
                case TOK_MINUS: func = "ml_sub"; break;
                case TOK_STAR: func = "ml_mul"; break;
                case TOK_SLASH: func = "ml_div"; break;
                case TOK_PERCENT: func = "ml_mod"; break;
                case TOK_EQEQ: func = "ml_eq"; break;
                case TOK_NEQ: func = "ml_neq"; break;
                case TOK_LT: func = "ml_lt"; break;
                case TOK_GT: func = "ml_gt"; break;
                case TOK_LTE: func = "ml_lte"; break;
                case TOK_GTE: func = "ml_gte"; break;
                case TOK_AND: func = "ml_and"; break;
                case TOK_OR: func = "ml_or"; break;
                default: fprintf(stderr, "C gen: unknown binary op %d\n", n->binary.op); exit(1);
            }
            char *r = new_temp(g);
            cg_emit(g, "    Value %s = %s(%s, %s);\n", r, func, left, right);
            free(left); free(right);
            return r;
        }
        case NODE_UNARY: {
            char *operand = compile_expr(g, n->unary.operand);
            char *r = new_temp(g);
            if (n->unary.op == TOK_MINUS)
                cg_emit(g, "    Value %s = ml_neg(%s);\n", r, operand);
            else
                cg_emit(g, "    Value %s = ml_not(%s);\n", r, operand);
            free(operand);
            return r;
        }
        case NODE_CALL: {
            if (strcmp(n->call.name, "len") == 0) {
                char *a = compile_expr(g, n->call.args[0]);
                char *r = new_temp(g);
                cg_emit(g, "    Value %s = ml_len(%s);\n", r, a);
                free(a); return r;
            }
            if (strcmp(n->call.name, "charAt") == 0) {
                char *a = compile_expr(g, n->call.args[0]);
                char *b = compile_expr(g, n->call.args[1]);
                char *r = new_temp(g);
                cg_emit(g, "    Value %s = ml_charat(%s, %s);\n", r, a, b);
                free(a); free(b); return r;
            }
            if (strcmp(n->call.name, "substr") == 0) {
                char *a = compile_expr(g, n->call.args[0]);
                char *b = compile_expr(g, n->call.args[1]);
                char *c = compile_expr(g, n->call.args[2]);
                char *r = new_temp(g);
                cg_emit(g, "    Value %s = ml_substr(%s, %s, %s);\n", r, a, b, c);
                free(a); free(b); free(c); return r;
            }
            if (strcmp(n->call.name, "toString") == 0) {
                char *a = compile_expr(g, n->call.args[0]);
                char *r = new_temp(g);
                cg_emit(g, "    Value %s = ml_tostring(%s);\n", r, a);
                free(a); return r;
            }
            if (strcmp(n->call.name, "toInt") == 0) {
                char *a = compile_expr(g, n->call.args[0]);
                char *r = new_temp(g);
                cg_emit(g, "    Value %s = ml_toint(%s);\n", r, a);
                free(a); return r;
            }
            if (strcmp(n->call.name, "strcmp") == 0) {
                char *a = compile_expr(g, n->call.args[0]);
                char *b = compile_expr(g, n->call.args[1]);
                char *r = new_temp(g);
                cg_emit(g, "    Value %s = ml_strcmp(%s, %s);\n", r, a, b);
                free(a); free(b); return r;
            }
            if (strcmp(n->call.name, "readAll") == 0) {
                char *r = new_temp(g);
                cg_emit(g, "    Value %s = ml_readall();\n", r);
                return r;
            }
            if (strcmp(n->call.name, "array") == 0) {
                char *a = compile_expr(g, n->call.args[0]);
                char *b = compile_expr(g, n->call.args[1]);
                char *r = new_temp(g);
                cg_emit(g, "    Value %s = ml_array_make(%s, %s);\n", r, a, b);
                free(a); free(b); return r;
            }
            if (strcmp(n->call.name, "argc") == 0) {
                char *r = new_temp(g);
                cg_emit(g, "    Value %s = ml_argc();\n", r);
                return r;
            }
            if (strcmp(n->call.name, "argv") == 0) {
                char *a = compile_expr(g, n->call.args[0]);
                char *r = new_temp(g);
                cg_emit(g, "    Value %s = ml_argv(%s);\n", r, a);
                free(a); return r;
            }
            if (strcmp(n->call.name, "readFile") == 0) {
                char *a = compile_expr(g, n->call.args[0]);
                char *r = new_temp(g);
                cg_emit(g, "    Value %s = ml_readfile(%s);\n", r, a);
                free(a); return r;
            }
            if (strcmp(n->call.name, "fileExists") == 0) {
                char *a = compile_expr(g, n->call.args[0]);
                char *r = new_temp(g);
                cg_emit(g, "    Value %s = ml_fileexists(%s);\n", r, a);
                free(a); return r;
            }
            if (strcmp(n->call.name, "writeFile") == 0) {
                char *a = compile_expr(g, n->call.args[0]);
                char *b = compile_expr(g, n->call.args[1]);
                char *r = new_temp(g);
                cg_emit(g, "    Value %s = ml_writefile(%s, %s);\n", r, a, b);
                free(a); free(b); return r;
            }
            if (strcmp(n->call.name, "system") == 0) {
                char *a = compile_expr(g, n->call.args[0]);
                char *r = new_temp(g);
                cg_emit(g, "    Value %s = ml_system(%s);\n", r, a);
                free(a); return r;
            }
            if (strcmp(n->call.name, "readLine") == 0) {
                char *r = new_temp(g);
                cg_emit(g, "    Value %s = ml_readline();\n", r);
                return r;
            }
            if (strcmp(n->call.name, "readFileBytes") == 0) {
                char *a = compile_expr(g, n->call.args[0]);
                char *r = new_temp(g);
                cg_emit(g, "    Value %s = ml_readfilebytes(%s);\n", r, a);
                free(a); return r;
            }
            if (strcmp(n->call.name, "writeFileBytes") == 0) {
                char *a = compile_expr(g, n->call.args[0]);
                char *b = compile_expr(g, n->call.args[1]);
                char *r = new_temp(g);
                cg_emit(g, "    Value %s = ml_writefilebytes(%s, %s);\n", r, a, b);
                free(a); free(b); return r;
            }
            if (strcmp(n->call.name, "chr") == 0) {
                char *a = compile_expr(g, n->call.args[0]);
                char *r = new_temp(g);
                cg_emit(g, "    Value %s = ml_chr(%s);\n", r, a);
                free(a); return r;
            }
            // User / native call. Resolve alias exactly as the IR backend did.
            const char *cname = n->call.name;
            for (int ai = 0; ai < g->parser->alias_count; ai++) {
                if (strcmp(g->parser->alias_names[ai], cname) == 0) {
                    cname = g->parser->alias_targets[ai]; break;
                }
            }
            int fi = -1;
            for (int i = 0; i < g->parser->func_count; i++) {
                if (strcmp(g->parser->funcs[i]->func.name, cname) == 0) { fi = i; break; }
            }
            if (fi < 0) { fprintf(stderr, "C gen: undefined function '%s'\n", cname); exit(1); }
            char **args = malloc(sizeof(char *) * (n->call.arg_count > 0 ? n->call.arg_count : 1));
            for (int i = 0; i < n->call.arg_count; i++) args[i] = compile_expr(g, n->call.args[i]);
            char *r = new_temp(g);
            int is_nat = g->parser->funcs[fi]->func.is_native;
            char label[256];
            if (is_nat) {
                const char *base = cname;
                const char *dot = strrchr(base, '.');
                if (dot) base = dot + 1;
                char cid[128]; cg_ident(base, cid, sizeof(cid));
                snprintf(label, sizeof(label), "ml_native_%s", cid);
            } else {
                char cid[128]; cg_ident(cname, cid, sizeof(cid));
                snprintf(label, sizeof(label), "user_%s", cid);
            }
            cg_emit(g, "    Value %s = %s(", r, label);
            for (int i = 0; i < n->call.arg_count; i++) {
                if (i > 0) cg_emit(g, ", ");
                cg_emit(g, "%s", args[i]);
            }
            cg_emit(g, ");\n");
            for (int i = 0; i < n->call.arg_count; i++) free(args[i]);
            free(args);
            return r;
        }
        case NODE_INDEX: {
            char *arr = compile_expr(g, n->index.array);
            char *idx = compile_expr(g, n->index.index);
            char *r = new_temp(g);
            cg_emit(g, "    Value %s = ml_array_get(%s, %s.payload);\n", r, arr, idx);
            free(arr); free(idx);
            return r;
        }
        case NODE_NIL: {
            char *r = new_temp(g);
            cg_emit(g, "    Value %s = ml_nil();\n", r);
            return r;
        }
        default:
            fprintf(stderr, "C gen: unexpected expr node %d\n", n->type);
            exit(1);
    }
}

char *generate_c_source(Parser *p) {
    CGen g;
    cg_buf_init(&g);
    g.parser = p;

    /* Module header */
    cg_emit(&g, "/* Generated by the minilang C backend. Do not edit. */\n");
    cg_emit(&g, "#include \"minilang_runtime.h\"\n");
    cg_emit(&g, "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <stdint.h>\n\n");
    cg_emit(&g, "/*__POOL__*/\n");

    /* Register top-level globals. */
    for (int gi = 0; gi < p->global_count; gi++) {
        Node *gvar = p->globals[gi];
        if (g.global_count >= g.global_cap) {
            g.global_cap = g.global_cap ? g.global_cap * 2 : 16;
            g.global_names = realloc(g.global_names, sizeof(char *) * g.global_cap);
            g.global_slots = realloc(g.global_slots, sizeof(char *) * g.global_cap);
        }
        g.global_names[g.global_count] = strdup(gvar->var_decl.name);
        char slot[256];
        snprintf(slot, sizeof(slot), "g%d", g.global_count);
        g.global_slots[g.global_count] = strdup(slot);
        g.global_count++;
    }
    for (int i = 0; i < g.global_count; i++)
        cg_emit(&g, "static Value %s;\n", g.global_slots[i]);
    cg_emit(&g, "\n");

    /* Forward declarations of user functions + native externs. */
    for (int fi = 0; fi < p->func_count; fi++) {
        Node *f = p->funcs[fi];
        if (f->func.is_native) continue;
        char cid[128]; cg_ident(f->func.name, cid, sizeof(cid));
        cg_emit(&g, "static Value user_%s(", cid);
        for (int i = 0; i < f->func.param_count; i++) {
            if (i > 0) cg_emit(&g, ", ");
            cg_emit(&g, "Value p%d", i);
        }
        cg_emit(&g, ");\n");
    }
    /* Dedup native externs by base name. */
    for (int fi = 0; fi < p->func_count; fi++) {
        Node *f = p->funcs[fi];
        if (!f->func.is_native) continue;
        const char *base = f->func.name;
        const char *dot = strrchr(base, '.');
        if (dot) base = dot + 1;
        char cid[128]; cg_ident(base, cid, sizeof(cid));
        int dup = 0;
        for (int dj = 0; dj < fi; dj++) {
            Node *prev = p->funcs[dj];
            if (!prev->func.is_native) continue;
            const char *pb = prev->func.name;
            const char *pd = strrchr(pb, '.');
            if (pd) pb = pd + 1;
            char pcid[128]; cg_ident(pb, pcid, sizeof(pcid));
            if (strcmp(pcid, cid) == 0) { dup = 1; break; }
        }
        if (dup) continue;
        cg_emit(&g, "extern Value ml_native_%s(", cid);
        for (int i = 0; i < f->func.param_count; i++) {
            if (i > 0) cg_emit(&g, ", ");
            cg_emit(&g, "Value");
        }
        cg_emit(&g, ");\n");
    }
    cg_emit(&g, "\n");

    /* User function definitions. */
    for (int fi = 0; fi < p->func_count; fi++) {
        Node *f = p->funcs[fi];
        if (f->func.is_native) continue;
        g.var_names = NULL; g.var_slots = NULL; g.var_count = 0; g.var_cap = 0;

        /* Register params and pre-scan locals. */
        for (int i = 0; i < f->func.param_count; i++) add_var(&g, f->func.params[i]);
        collect_vars(&g, f->func.body);

        char cid[128]; cg_ident(f->func.name, cid, sizeof(cid));
        cg_emit(&g, "static Value user_%s(", cid);
        for (int i = 0; i < f->func.param_count; i++) {
            if (i > 0) cg_emit(&g, ", ");
            cg_emit(&g, "Value p%d", i);
        }
        cg_emit(&g, ") {\n");
        /* Local slot declarations. */
        for (int i = 0; i < g.var_count; i++)
            cg_emit(&g, "    Value %s;\n", g.var_slots[i]);
        /* Copy params into their slots. */
        for (int i = 0; i < f->func.param_count; i++) {
            int idx = find_var(&g, f->func.params[i]);
            cg_emit(&g, "    %s = p%d;\n", g.var_slots[idx], i);
        }
        /* Initialize top-level globals in main, matching the IR backend. */
        if (strcmp(f->func.name, "main") == 0) {
            for (int gi = 0; gi < p->global_count; gi++) {
                Node *gvar = p->globals[gi];
                int gidx = find_global(&g, gvar->var_decl.name);
                char *val = compile_expr(&g, gvar->var_decl.value);
                cg_emit(&g, "    %s = %s;\n", g.global_slots[gidx], val);
                free(val);
            }
        }
        compile_stmt(&g, f->func.body);
        /* If the body's final statement is an explicit return, do not append a
         * redundant trailing nil return. */
        int ends_return = 0;
        if (f->func.body && f->func.body->type == NODE_BLOCK && f->func.body->block.count > 0) {
            Node *last = f->func.body->block.stmts[f->func.body->block.count - 1];
            if (last->type == NODE_RETURN) ends_return = 1;
        } else if (f->func.body && f->func.body->type == NODE_RETURN) {
            ends_return = 1;
        }
        if (!ends_return) cg_emit(&g, "    return ml_nil();\n");
        cg_emit(&g, "}\n\n");

        for (int i = 0; i < g.var_count; i++) {
            free(g.var_names[i]);
            free(g.var_slots[i]);
        }
        free(g.var_names);
        free(g.var_slots);
    }

    /* C main entry. */
    cg_emit(&g, "int main(int argc, char **argv) {\n");
    cg_emit(&g, "    ml_set_args(argc, argv);\n");
    cg_emit(&g, "    (void)user_main();\n");
    cg_emit(&g, "    return 0;\n");
    cg_emit(&g, "}\n");

    /* Place the string constant pool at the placeholder slot near the top. */
    if (g.strpool_len > 0) {
        char *marker = strstr(g.buf, "/*__POOL__*/");
        int insert_pos = marker ? (int)(marker - g.buf) : g.len;
        int tail_len = g.len - insert_pos;
        cg_reserve(&g, g.strpool_len + 1);
        char *tail = malloc(tail_len > 0 ? tail_len : 1);
        memcpy(tail, g.buf + insert_pos, tail_len);
        memcpy(g.buf + insert_pos, g.strpool, g.strpool_len);
        g.buf[insert_pos + g.strpool_len] = '\n';
        memcpy(g.buf + insert_pos + g.strpool_len + 1, tail, tail_len);
        g.len += g.strpool_len + 1;
        g.buf[g.len] = '\0';
        free(tail);
    }
    /* Drop the placeholder comment when there were no string constants. */
    {
        char *marker = strstr(g.buf, "/*__POOL__*/\n");
        if (marker) {
            int pos = (int)(marker - g.buf);
            memmove(marker, marker + strlen("/*__POOL__*/\n"), g.len - pos - strlen("/*__POOL__*/\n") + 1);
            g.len -= strlen("/*__POOL__*/\n");
            g.buf[g.len] = '\0';
        }
    }

    for (int i = 0; i < g.global_count; i++) {
        free(g.global_names[i]);
        free(g.global_slots[i]);
    }
    free(g.global_names);
    free(g.global_slots);
    free(g.strpool);
    return g.buf;
}
