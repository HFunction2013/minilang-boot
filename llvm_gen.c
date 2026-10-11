#include "minilang.h"

/* ===================== LLVM IR Generator ===================== */
/* Value = { i64 tag, i64 payload }
   tag: 0=nil, 1=int, 2=string(ptr), 3=array(ptr)
   Array = { i64 len, %Value* data }
*/

typedef struct {
    char *buf;
    int len;
    int cap;
    int temp_counter;
    int block_counter;
    Parser *parser;
    // Current function variable tracking
    char **var_names;
    char **var_regs;  // alloca register names for locals
    int var_count;
    int var_cap;
    int func_idx;
    // For collecting allocas in entry block
    char *allocas;
    int allocas_len;
    int allocas_cap;
    // String global constants
    char *globals;
    int globals_len;
    int globals_cap;
    // Top-level global variables (module-level)
    char **global_names;
    char **global_ptrs;  // e.g. "@global_TOK_VAR"
    int global_count;
    int global_cap;
    // Break target stack (innermost loop exit block)
    char **break_targets;
    int break_count;
    int break_cap;
} IRGen;

static void ir_buf_init(IRGen *g) {
    g->buf = malloc(4096);
    g->buf[0] = '\0';
    g->len = 0; g->cap = 4096;
    g->temp_counter = 0;
    g->block_counter = 0;
    g->allocas = malloc(2048);
    g->allocas[0] = '\0';
    g->allocas_len = 0; g->allocas_cap = 2048;
    g->globals = malloc(4096);
    g->globals[0] = '\0';
    g->globals_len = 0; g->globals_cap = 4096;
    g->global_names = NULL; g->global_ptrs = NULL;
    g->global_count = 0; g->global_cap = 0;
    g->break_targets = NULL; g->break_count = 0; g->break_cap = 0;
}

static void ir_emit_global(IRGen *g, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (g->globals_len + needed + 1 > g->globals_cap) {
        while (g->globals_len + needed + 1 > g->globals_cap) g->globals_cap *= 2;
        g->globals = realloc(g->globals, g->globals_cap);
    }
    va_start(ap, fmt);
    vsnprintf(g->globals + g->globals_len, needed + 1, fmt, ap);
    va_end(ap);
    g->globals_len += needed;
}

/* Ensure room for `extra` more bytes (plus the terminating NUL) */
static void ir_reserve(IRGen *g, int extra) {
    if (g->len + extra + 1 > g->cap) {
        while (g->len + extra + 1 > g->cap) g->cap *= 2;
        g->buf = realloc(g->buf, g->cap);
    }
}

static void ir_emit(IRGen *g, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (g->len + needed + 1 > g->cap) {
        while (g->len + needed + 1 > g->cap) g->cap *= 2;
        g->buf = realloc(g->buf, g->cap);
    }
    va_start(ap, fmt);
    vsnprintf(g->buf + g->len, needed + 1, fmt, ap);
    va_end(ap);
    g->len += needed;
}

static void ir_emit_alloca(IRGen *g, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (g->allocas_len + needed + 1 > g->allocas_cap) {
        while (g->allocas_len + needed + 1 > g->allocas_cap) g->allocas_cap *= 2;
        g->allocas = realloc(g->allocas, g->allocas_cap);
    }
    va_start(ap, fmt);
    vsnprintf(g->allocas + g->allocas_len, needed + 1, fmt, ap);
    va_end(ap);
    g->allocas_len += needed;
}

static char *new_temp(IRGen *g) {
    static char name[32];
    snprintf(name, sizeof(name), "%%t%d", g->temp_counter++);
    return strdup(name);
}

static char *new_block(IRGen *g) {
    static char name[32];
    snprintf(name, sizeof(name), "L%d", g->block_counter++);
    return strdup(name);
}

static int find_var_ir(IRGen *g, const char *name) {
    for (int i = 0; i < g->var_count; i++)
        if (strcmp(g->var_names[i], name) == 0) return i;
    return -1;
}

static int find_global_ir(IRGen *g, const char *name) {
    for (int i = 0; i < g->global_count; i++)
        if (strcmp(g->global_names[i], name) == 0) return i;
    return -1;
}

static void push_break_target(IRGen *g, const char *label) {
    if (g->break_count >= g->break_cap) {
        g->break_cap = g->break_cap ? g->break_cap * 2 : 8;
        g->break_targets = realloc(g->break_targets, sizeof(char*) * g->break_cap);
    }
    g->break_targets[g->break_count++] = strdup(label);
}

static void pop_break_target(IRGen *g) {
    if (g->break_count > 0) {
        free(g->break_targets[--g->break_count]);
    }
}

static int add_var_ir(IRGen *g, const char *name) {
    int existing = find_var_ir(g, name);
    if (existing >= 0) return existing;
    if (g->var_count >= g->var_cap) {
        g->var_cap = g->var_cap ? g->var_cap * 2 : 16;
        g->var_names = realloc(g->var_names, sizeof(char*) * g->var_cap);
        g->var_regs = realloc(g->var_regs, sizeof(char*) * g->var_cap);
    }
    g->var_names[g->var_count] = strdup(name);
    char *reg = new_temp(g);
    g->var_regs[g->var_count] = reg;
    ir_emit_alloca(g, "  %s = alloca %%Value, align 8\n", reg);
    return g->var_count++;
}

static char *compile_expr_ir(IRGen *g, Node *n);

static void compile_stmt_ir(IRGen *g, Node *n) {
    if (!n) return;
    switch (n->type) {
        case NODE_VAR_DECL: {
            int idx = add_var_ir(g, n->var_decl.name);
            char *val = compile_expr_ir(g, n->var_decl.value);
            ir_emit(g, "  store %%Value %s, %%Value* %s, align 8\n", val, g->var_regs[idx]);
            free(val);
            break;
        }
        case NODE_ASSIGN: {
            int idx = find_var_ir(g, n->assign.name);
            if (idx >= 0) {
                char *val = compile_expr_ir(g, n->assign.value);
                ir_emit(g, "  store %%Value %s, %%Value* %s, align 8\n", val, g->var_regs[idx]);
                free(val);
                break;
            }
            int gidx = find_global_ir(g, n->assign.name);
            if (gidx >= 0) {
                char *val = compile_expr_ir(g, n->assign.value);
                ir_emit(g, "  store %%Value %s, %%Value* %s, align 8\n", val, g->global_ptrs[gidx]);
                free(val);
                break;
            }
            fprintf(stderr, "IR error: undefined var '%s'\n", n->assign.name); exit(1);
        }
        case NODE_INDEX_ASSIGN: {
            char *arr = compile_expr_ir(g, n->index_assign.target);
            char *idx = compile_expr_ir(g, n->index_assign.index);
            char *val = compile_expr_ir(g, n->index_assign.value);
            // Extract index payload
            char *idxval = new_temp(g);
            ir_emit(g, "  %s = extractvalue %%Value %s, 1\n", idxval, idx);
            ir_emit(g, "  call void @ml_array_set(%%Value %s, i64 %s, %%Value %s)\n", arr, idxval, val);
            free(arr); free(idx); free(val); free(idxval);
            break;
        }
        case NODE_IF: {
            char *cond = compile_expr_ir(g, n->if_stmt.cond);
            char *condval = new_temp(g);
            ir_emit(g, "  %s = extractvalue %%Value %s, 1\n", condval, cond);
            char *condbool = new_temp(g);
            ir_emit(g, "  %s = icmp ne i64 %s, 0\n", condbool, condval);
            char *then_b = new_block(g);
            char *else_b = new_block(g);
            char *merge_b = new_block(g);
            if (n->if_stmt.else_branch) {
                ir_emit(g, "  br i1 %s, label %%%s, label %%%s\n", condbool, then_b, else_b);
                ir_emit(g, "%s:\n", then_b);
                compile_stmt_ir(g, n->if_stmt.then_branch);
                ir_emit(g, "  br label %%%s\n", merge_b);
                ir_emit(g, "%s:\n", else_b);
                compile_stmt_ir(g, n->if_stmt.else_branch);
                ir_emit(g, "  br label %%%s\n", merge_b);
                ir_emit(g, "%s:\n", merge_b);
            } else {
                ir_emit(g, "  br i1 %s, label %%%s, label %%%s\n", condbool, then_b, merge_b);
                ir_emit(g, "%s:\n", then_b);
                compile_stmt_ir(g, n->if_stmt.then_branch);
                ir_emit(g, "  br label %%%s\n", merge_b);
                ir_emit(g, "%s:\n", merge_b);
            }
            free(cond); free(condval); free(condbool);
            free(then_b); free(else_b); free(merge_b);
            break;
        }
        case NODE_WHILE: {
            char *cond_b = new_block(g);
            char *body_b = new_block(g);
            char *end_b = new_block(g);
            push_break_target(g, end_b);
            ir_emit(g, "  br label %%%s\n", cond_b);
            ir_emit(g, "%s:\n", cond_b);
            char *cond = compile_expr_ir(g, n->while_stmt.cond);
            char *condval = new_temp(g);
            ir_emit(g, "  %s = extractvalue %%Value %s, 1\n", condval, cond);
            char *condbool = new_temp(g);
            ir_emit(g, "  %s = icmp ne i64 %s, 0\n", condbool, condval);
            ir_emit(g, "  br i1 %s, label %%%s, label %%%s\n", condbool, body_b, end_b);
            ir_emit(g, "%s:\n", body_b);
            compile_stmt_ir(g, n->while_stmt.body);
            ir_emit(g, "  br label %%%s\n", cond_b);
            ir_emit(g, "%s:\n", end_b);
            pop_break_target(g);
            free(cond); free(condval); free(condbool);
            free(cond_b); free(body_b); free(end_b);
            break;
        }
        case NODE_RETURN: {
            if (n->return_stmt.value) {
                char *val = compile_expr_ir(g, n->return_stmt.value);
                ir_emit(g, "  ret %%Value %s\n", val);
                free(val);
            } else {
                ir_emit(g, "  ret %%Value { i64 0, i64 0 }\n");
            }
            break;
        }
        case NODE_BREAK: {
            if (g->break_count <= 0) { fprintf(stderr, "IR error: break outside loop\n"); exit(1); }
            ir_emit(g, "  br label %%%s\n", g->break_targets[g->break_count - 1]);
            break;
        }
        case NODE_PRINT: {
            char *val = compile_expr_ir(g, n->print.value);
            ir_emit(g, "  call void @ml_print(%%Value %s, i32 %d)\n", val, n->print.newline ? 1 : 0);
            free(val);
            break;
        }
        case NODE_BLOCK: {
            for (int i = 0; i < n->block.count; i++)
                compile_stmt_ir(g, n->block.stmts[i]);
            break;
        }
        case NODE_EXPR_STMT: {
            char *val = compile_expr_ir(g, n->expr_stmt.expr);
            (void)val; // result discarded
            free(val);
            break;
        }
        default:
            fprintf(stderr, "IR error: unexpected stmt node %d\n", n->type);
            exit(1);
    }
}

static char *compile_expr_ir(IRGen *g, Node *n) {
    switch (n->type) {
        case NODE_INT: {
            char *r = new_temp(g);
            ir_emit(g, "  %s = insertvalue %%Value { i64 1, i64 0 }, i64 %lld, 1\n", r, (long long)n->integer.value);
            return r;
        }
        case NODE_STRING: {
            // Create global string constant
            static int str_counter = 0;
            char gname[32];
            snprintf(gname, sizeof(gname), "@.str%d", str_counter++);
            // Escape the string for LLVM IR
            char *escaped = malloc(strlen(n->string.value) * 4 + 1);
            int ei = 0;
            for (int i = 0; n->string.value[i]; i++) {
                unsigned char c = n->string.value[i];
                if (c == '"') {
                    escaped[ei++] = '\\'; escaped[ei++] = '2'; escaped[ei++] = '2';
                } else if (c == '\\') {
                    escaped[ei++] = '\\'; escaped[ei++] = '5'; escaped[ei++] = 'C';
                } else if (c == '\n') {
                    escaped[ei++] = '\\'; escaped[ei++] = '0'; escaped[ei++] = 'A';
                } else if (c == '\t') {
                    escaped[ei++] = '\\'; escaped[ei++] = '0'; escaped[ei++] = '9';
                } else if (c < 32 || c >= 127) {
                    ei += snprintf(escaped + ei, 8, "\\%02X", c);
                } else {
                    escaped[ei++] = c;
                }
            }
            escaped[ei] = '\0';
            int slen = (int)strlen(n->string.value) + 1;
            ir_emit_global(g, "%s = private unnamed_addr constant [%d x i8] c\"%s\\00\", align 1\n",
                            gname, slen, escaped);
            free(escaped);

            // Get pointer to string
            char *ptr = new_temp(g);
            ir_emit(g, "  %s = getelementptr inbounds [%d x i8], [%d x i8]* %s, i64 0, i64 0\n", ptr, slen, slen, gname);
            // Bitcast to i64
            char *iptr = new_temp(g);
            ir_emit(g, "  %s = ptrtoint i8* %s to i64\n", iptr, ptr);
            // Build Value
            char *r = new_temp(g);
            ir_emit(g, "  %s = insertvalue %%Value { i64 2, i64 0 }, i64 %s, 1\n", r, iptr);
            free(ptr); free(iptr);
            return r;
        }
        case NODE_ARRAY_LIT: {
            int count = n->array_lit.count;
            // Compile all elements first
            char **elems = malloc(sizeof(char*) * count);
            for (int i = 0; i < count; i++) elems[i] = compile_expr_ir(g, n->array_lit.items[i]);
            // Allocate temp array on stack for the items
            char *arrptr = new_temp(g);
            int acount = count > 0 ? count : 1;
            ir_emit(g, "  %s = alloca [%d x %%Value], align 8\n", arrptr, acount);
            for (int i = 0; i < count; i++) {
                char *elemptr = new_temp(g);
                ir_emit(g, "  %s = getelementptr inbounds [%d x %%Value], [%d x %%Value]* %s, i64 0, i64 %d\n",
                         elemptr, acount, acount, arrptr, i);
                ir_emit(g, "  store %%Value %s, %%Value* %s, align 8\n", elems[i], elemptr);
                free(elemptr);
            }
            char *dataptr = new_temp(g);
            ir_emit(g, "  %s = getelementptr inbounds [%d x %%Value], [%d x %%Value]* %s, i64 0, i64 0\n",
                     dataptr, acount, acount, arrptr);
            char *r = new_temp(g);
            ir_emit(g, "  %s = call %%Value @ml_array_create(i64 %d, %%Value* %s)\n", r, count, dataptr);
            for (int i = 0; i < count; i++) free(elems[i]);
            free(elems); free(arrptr); free(dataptr);
            return r;
        }
        case NODE_VAR: {
            int idx = find_var_ir(g, n->var.name);
            if (idx >= 0) {
                char *r = new_temp(g);
                ir_emit(g, "  %s = load %%Value, %%Value* %s, align 8\n", r, g->var_regs[idx]);
                return r;
            }
            int gidx = find_global_ir(g, n->var.name);
            if (gidx >= 0) {
                char *r = new_temp(g);
                ir_emit(g, "  %s = load %%Value, %%Value* %s, align 8\n", r, g->global_ptrs[gidx]);
                return r;
            }
            fprintf(stderr, "IR error: undefined var '%s'\n", n->var.name); exit(1);
        }
        case NODE_BINARY: {
            char *left = compile_expr_ir(g, n->binary.left);
            char *right = compile_expr_ir(g, n->binary.right);
            char *r = new_temp(g);
            const char *func = NULL;
            switch (n->binary.op) {
                case TOK_PLUS: func = "@ml_add"; break;
                case TOK_MINUS: func = "@ml_sub"; break;
                case TOK_STAR: func = "@ml_mul"; break;
                case TOK_SLASH: func = "@ml_div"; break;
                case TOK_PERCENT: func = "@ml_mod"; break;
                case TOK_EQEQ: func = "@ml_eq"; break;
                case TOK_NEQ: func = "@ml_neq"; break;
                case TOK_LT: func = "@ml_lt"; break;
                case TOK_GT: func = "@ml_gt"; break;
                case TOK_LTE: func = "@ml_lte"; break;
                case TOK_GTE: func = "@ml_gte"; break;
                case TOK_AND: func = "@ml_and"; break;
                case TOK_OR: func = "@ml_or"; break;
                default: fprintf(stderr, "IR error: unknown binary op %d\n", n->binary.op); exit(1);
            }
            ir_emit(g, "  %s = call %%Value %s(%%Value %s, %%Value %s)\n", r, func, left, right);
            free(left); free(right);
            return r;
        }
        case NODE_UNARY: {
            char *operand = compile_expr_ir(g, n->unary.operand);
            char *r = new_temp(g);
            if (n->unary.op == TOK_MINUS)
                ir_emit(g, "  %s = call %%Value @ml_neg(%%Value %s)\n", r, operand);
            else
                ir_emit(g, "  %s = call %%Value @ml_not(%%Value %s)\n", r, operand);
            free(operand);
            return r;
        }
        case NODE_CALL: {
            // Built-ins
            if (strcmp(n->call.name, "len") == 0) {
                char *a = compile_expr_ir(g, n->call.args[0]);
                char *r = new_temp(g);
                ir_emit(g, "  %s = call %%Value @ml_len(%%Value %s)\n", r, a);
                free(a); return r;
            }
            if (strcmp(n->call.name, "charAt") == 0) {
                char *a = compile_expr_ir(g, n->call.args[0]);
                char *b = compile_expr_ir(g, n->call.args[1]);
                char *r = new_temp(g);
                ir_emit(g, "  %s = call %%Value @ml_charat(%%Value %s, %%Value %s)\n", r, a, b);
                free(a); free(b); return r;
            }
            if (strcmp(n->call.name, "substr") == 0) {
                char *a = compile_expr_ir(g, n->call.args[0]);
                char *b = compile_expr_ir(g, n->call.args[1]);
                char *c = compile_expr_ir(g, n->call.args[2]);
                char *r = new_temp(g);
                ir_emit(g, "  %s = call %%Value @ml_substr(%%Value %s, %%Value %s, %%Value %s)\n", r, a, b, c);
                free(a); free(b); free(c); return r;
            }
            if (strcmp(n->call.name, "toString") == 0) {
                char *a = compile_expr_ir(g, n->call.args[0]);
                char *r = new_temp(g);
                ir_emit(g, "  %s = call %%Value @ml_tostring(%%Value %s)\n", r, a);
                free(a); return r;
            }
            if (strcmp(n->call.name, "toInt") == 0) {
                char *a = compile_expr_ir(g, n->call.args[0]);
                char *r = new_temp(g);
                ir_emit(g, "  %s = call %%Value @ml_toint(%%Value %s)\n", r, a);
                free(a); return r;
            }
            if (strcmp(n->call.name, "strcmp") == 0) {
                char *a = compile_expr_ir(g, n->call.args[0]);
                char *b = compile_expr_ir(g, n->call.args[1]);
                char *r = new_temp(g);
                ir_emit(g, "  %s = call %%Value @ml_strcmp(%%Value %s, %%Value %s)\n", r, a, b);
                free(a); free(b); return r;
            }
            if (strcmp(n->call.name, "readAll") == 0) {
                char *r = new_temp(g);
                ir_emit(g, "  %s = call %%Value @ml_readall()\n", r);
                return r;
            }
            if (strcmp(n->call.name, "array") == 0) {
                char *a = compile_expr_ir(g, n->call.args[0]);
                char *b = compile_expr_ir(g, n->call.args[1]);
                char *r = new_temp(g);
                ir_emit(g, "  %s = call %%Value @ml_array_make(%%Value %s, %%Value %s)\n", r, a, b);
                free(a); free(b); return r;
            }
            if (strcmp(n->call.name, "argc") == 0) {
                char *r = new_temp(g);
                ir_emit(g, "  %s = call %%Value @ml_argc()\n", r);
                return r;
            }
            if (strcmp(n->call.name, "argv") == 0) {
                char *a = compile_expr_ir(g, n->call.args[0]);
                char *r = new_temp(g);
                ir_emit(g, "  %s = call %%Value @ml_argv(%%Value %s)\n", r, a);
                free(a); return r;
            }
            if (strcmp(n->call.name, "readFile") == 0) {
                char *a = compile_expr_ir(g, n->call.args[0]);
                char *r = new_temp(g);
                ir_emit(g, "  %s = call %%Value @ml_readfile(%%Value %s)\n", r, a);
                free(a); return r;
            }
            if (strcmp(n->call.name, "fileExists") == 0) {
                char *a = compile_expr_ir(g, n->call.args[0]);
                char *r = new_temp(g);
                ir_emit(g, "  %s = call %%Value @ml_fileexists(%%Value %s)\n", r, a);
                free(a); return r;
            }
            if (strcmp(n->call.name, "writeFile") == 0) {
                char *a = compile_expr_ir(g, n->call.args[0]); // path
                char *b = compile_expr_ir(g, n->call.args[1]); // content
                char *r = new_temp(g);
                ir_emit(g, "  %s = call %%Value @ml_writefile(%%Value %s, %%Value %s)\n", r, a, b);
                free(a); free(b); return r;
            }
            if (strcmp(n->call.name, "system") == 0) {
                char *a = compile_expr_ir(g, n->call.args[0]);
                char *r = new_temp(g);
                ir_emit(g, "  %s = call %%Value @ml_system(%%Value %s)\n", r, a);
                free(a); return r;
            }
            if (strcmp(n->call.name, "readLine") == 0) {
                char *r = new_temp(g);
                ir_emit(g, "  %s = call %%Value @ml_readline()\n", r);
                return r;
            }
            if (strcmp(n->call.name, "readFileBytes") == 0) {
                char *a = compile_expr_ir(g, n->call.args[0]);
                char *r = new_temp(g);
                ir_emit(g, "  %s = call %%Value @ml_readfilebytes(%%Value %s)\n", r, a);
                free(a); return r;
            }
            if (strcmp(n->call.name, "writeFileBytes") == 0) {
                char *a = compile_expr_ir(g, n->call.args[0]); // path
                char *b = compile_expr_ir(g, n->call.args[1]); // byte array
                char *r = new_temp(g);
                ir_emit(g, "  %s = call %%Value @ml_writefilebytes(%%Value %s, %%Value %s)\n", r, a, b);
                free(a); free(b); return r;
            }
            if (strcmp(n->call.name, "chr") == 0) {
                char *a = compile_expr_ir(g, n->call.args[0]);
                char *r = new_temp(g);
                ir_emit(g, "  %s = call %%Value @ml_chr(%%Value %s)\n", r, a);
                free(a); return r;
            }
            // User function call
            // Resolve alias
            const char *cname = n->call.name;
            for (int ai = 0; ai < g->parser->alias_count; ai++) {
                if (strcmp(g->parser->alias_names[ai], cname) == 0) { cname = g->parser->alias_targets[ai]; break; }
            }
            // Find function index
            int fi = -1;
            for (int i = 0; i < g->parser->func_count; i++) {
                if (strcmp(g->parser->funcs[i]->func.name, cname) == 0) { fi = i; break; }
            }
            if (fi < 0) { fprintf(stderr, "IR error: undefined function '%s'\n", cname); exit(1); }
            char **args = malloc(sizeof(char*) * n->call.arg_count);
            for (int i = 0; i < n->call.arg_count; i++) args[i] = compile_expr_ir(g, n->call.args[i]);
            char *r = new_temp(g);
            // Build call
            char callbuf[2048];
            int pos = snprintf(callbuf, sizeof(callbuf), "  %s = call %%Value @user_%s(", r, cname);
            for (int i = 0; i < n->call.arg_count; i++) {
                if (i > 0) pos += snprintf(callbuf + pos, sizeof(callbuf) - pos, ", ");
                pos += snprintf(callbuf + pos, sizeof(callbuf) - pos, "%%Value %s", args[i]);
            }
            pos += snprintf(callbuf + pos, sizeof(callbuf) - pos, ")\n");
            ir_emit(g, "%s", callbuf);
            for (int i = 0; i < n->call.arg_count; i++) free(args[i]);
            free(args);
            return r;
        }
        case NODE_INDEX: {
            char *arr = compile_expr_ir(g, n->index.array);
            char *idx = compile_expr_ir(g, n->index.index);
            char *idxval = new_temp(g);
            ir_emit(g, "  %s = extractvalue %%Value %s, 1\n", idxval, idx);
            char *r = new_temp(g);
            ir_emit(g, "  %s = call %%Value @ml_array_get(%%Value %s, i64 %s)\n", r, arr, idxval);
            free(arr); free(idx); free(idxval);
            return r;
        }
        case NODE_NIL: {
            char *r = new_temp(g);
            ir_emit(g, "  %s = insertvalue %%Value { i64 0, i64 0 }, i64 0, 1\n", r);
            return r;
        }
        default:
            fprintf(stderr, "IR error: unexpected expr node %d\n", n->type);
            exit(1);
    }
}

char *generate_llvm_ir(Parser *p) {
    IRGen g;
    ir_buf_init(&g);
    g.parser = p;
    g.var_names = NULL; g.var_regs = NULL; g.var_count = 0; g.var_cap = 0;

    // Module header
    ir_emit(&g, ";; Generated by minilang LLVM backend\n");
    ir_emit(&g, "%%Value = type { i64, i64 }\n");
    ir_emit(&g, "%%Array = type { i64, %%Value* }\n\n");

    // External runtime declarations
    ir_emit(&g, "declare void @ml_print(%%Value, i32)\n");
    ir_emit(&g, "declare %%Value @ml_add(%%Value, %%Value)\n");
    ir_emit(&g, "declare %%Value @ml_sub(%%Value, %%Value)\n");
    ir_emit(&g, "declare %%Value @ml_mul(%%Value, %%Value)\n");
    ir_emit(&g, "declare %%Value @ml_div(%%Value, %%Value)\n");
    ir_emit(&g, "declare %%Value @ml_mod(%%Value, %%Value)\n");
    ir_emit(&g, "declare %%Value @ml_neg(%%Value)\n");
    ir_emit(&g, "declare %%Value @ml_eq(%%Value, %%Value)\n");
    ir_emit(&g, "declare %%Value @ml_neq(%%Value, %%Value)\n");
    ir_emit(&g, "declare %%Value @ml_lt(%%Value, %%Value)\n");
    ir_emit(&g, "declare %%Value @ml_gt(%%Value, %%Value)\n");
    ir_emit(&g, "declare %%Value @ml_lte(%%Value, %%Value)\n");
    ir_emit(&g, "declare %%Value @ml_gte(%%Value, %%Value)\n");
    ir_emit(&g, "declare %%Value @ml_and(%%Value, %%Value)\n");
    ir_emit(&g, "declare %%Value @ml_or(%%Value, %%Value)\n");
    ir_emit(&g, "declare %%Value @ml_not(%%Value)\n");
    ir_emit(&g, "declare %%Value @ml_array_create(i64, %%Value*)\n");
    ir_emit(&g, "declare %%Value @ml_array_make(%%Value, %%Value)\n");
    ir_emit(&g, "declare %%Value @ml_argc()\n");
    ir_emit(&g, "declare %%Value @ml_argv(%%Value)\n");
    ir_emit(&g, "declare %%Value @ml_readfile(%%Value)\n");
    ir_emit(&g, "declare %%Value @ml_fileexists(%%Value)\n");
    ir_emit(&g, "declare %%Value @ml_writefile(%%Value, %%Value)\n");
    ir_emit(&g, "declare %%Value @ml_system(%%Value)\n");
    ir_emit(&g, "declare %%Value @ml_readline()\n");
    ir_emit(&g, "declare %%Value @ml_readfilebytes(%%Value)\n");
    ir_emit(&g, "declare %%Value @ml_writefilebytes(%%Value, %%Value)\n");
    ir_emit(&g, "declare %%Value @ml_chr(%%Value)\n");
    ir_emit(&g, "declare void @ml_set_args(i32, i8**)\n");
    ir_emit(&g, "declare %%Value @ml_array_get(%%Value, i64)\n");
    ir_emit(&g, "declare void @ml_array_set(%%Value, i64, %%Value)\n");
    ir_emit(&g, "declare %%Value @ml_len(%%Value)\n");
    ir_emit(&g, "declare %%Value @ml_charat(%%Value, %%Value)\n");
    ir_emit(&g, "declare %%Value @ml_substr(%%Value, %%Value, %%Value)\n");
    ir_emit(&g, "declare %%Value @ml_tostring(%%Value)\n");
    ir_emit(&g, "declare %%Value @ml_toint(%%Value)\n");
    ir_emit(&g, "declare %%Value @ml_strcmp(%%Value, %%Value)\n");
    ir_emit(&g, "declare %%Value @ml_readall()\n\n");

    // We'll collect string globals separately and prepend them.
    // Since compile_expr_ir uses a static globals buffer, we need to reset it.
    // Actually, let's just emit functions and then prepend globals.
    // The static buffer in compile_expr_ir will accumulate strings.

    // Register top-level global variables and emit their definitions
    for (int gi = 0; gi < p->global_count; gi++) {
        Node *gvar = p->globals[gi];
        if (g.global_count >= g.global_cap) {
            g.global_cap = g.global_cap ? g.global_cap * 2 : 16;
            g.global_names = realloc(g.global_names, sizeof(char*) * g.global_cap);
            g.global_ptrs = realloc(g.global_ptrs, sizeof(char*) * g.global_cap);
        }
        g.global_names[g.global_count] = strdup(gvar->var_decl.name);
        char ptrname[256];
        snprintf(ptrname, sizeof(ptrname), "@global_%s", gvar->var_decl.name);
        g.global_ptrs[g.global_count] = strdup(ptrname);
        ir_emit_global(&g, "%s = global %%Value zeroinitializer, align 8\n", ptrname);
        g.global_count++;
    }

    // Generate each user function
    for (int fi = 0; fi < p->func_count; fi++) {
        Node *f = p->funcs[fi];
        g.var_names = NULL; g.var_regs = NULL; g.var_count = 0; g.var_cap = 0;
        g.func_idx = fi;
        g.allocas[0] = '\0'; g.allocas_len = 0;

        // Function signature
        char sigbuf[1024];
        int pos = snprintf(sigbuf, sizeof(sigbuf), "define %%Value @user_%s(", f->func.name);
        for (int i = 0; i < f->func.param_count; i++) {
            if (i > 0) pos += snprintf(sigbuf + pos, sizeof(sigbuf) - pos, ", ");
            pos += snprintf(sigbuf + pos, sizeof(sigbuf) - pos, "%%Value %%p%d", i);
        }
        pos += snprintf(sigbuf + pos, sizeof(sigbuf) - pos, ") {\n");
        ir_emit(&g, "%s", sigbuf);

        // Entry block - allocas will be inserted here
        ir_emit(&g, "entry:\n");
        int entry_pos = g.len; // position right after "entry:\n"

        // Add params as vars and store them
        for (int i = 0; i < f->func.param_count; i++) {
            int idx = add_var_ir(&g, f->func.params[i]);
            ir_emit(&g, "  store %%Value %%p%d, %%Value* %s, align 8\n", i, g.var_regs[idx]);
        }

        // Now compile body - but we need allocas to be in entry block.
        // The problem: var declarations inside the body create allocas via ir_emit_alloca,
        // but those need to be in the entry block. We collect them in g.allocas and
        // need to insert them before the body code.
        // Strategy: compile body into a temp buffer, then emit allocas + body.
        // But ir_emit appends to g.buf directly. Let's use a different approach:
        // compile body, then move all alloca instructions to the top.
        // Actually, LLVM allows allocas anywhere in the function, they just need to be
        // in the first block for optimal code. For correctness, they can be anywhere.
        // But var declarations inside if/while blocks would create allocas in those blocks,
        // which is valid LLVM. However, the alloca register name would be defined in
        // a branch and used later - that's a problem for SSA.
        //
        // Better approach: pre-allocate all vars. But we don't know all vars ahead of time.
        // Simplest correct approach: collect all var names by walking the AST first,
        // then emit all allocas in entry, then compile body.
        //
        // Let me do a pre-scan of the function body to find all var declarations.

        // For now, let's just emit allocas inline. LLVM actually allows alloca in any block,
        // and the pointer is valid for the entire function. The SSA issue is that the alloca
        // result (a pointer) is defined in one block and used in another. But pointers from
        // alloca are special - LLVM allows them to be used in any block because they're
        // guaranteed to be in the stack frame. Actually no, in SSA form, if the alloca is
        // in a conditional block, the pointer value might not be defined on all paths.
        //
        // To be safe, let me pre-scan for vars. I'll write a quick function to collect
        // all var declarations in a function body.

        // Actually, the simplest fix: emit all allocas at the start by doing a pre-scan.
        // Let me collect var names from the body, add them all, then compile.

        // Pre-scan for var declarations (already done above for params)
        // We need to scan the body for NODE_VAR_DECL nodes.
        // Let me do a quick recursive scan.

        // Since this is getting complex, let me use a simpler approach:
        // Just compile the body. Allocas emitted via ir_emit_alloca go into g.allocas.
        // After compiling the body, we'll have the body in g.buf (after "entry:\n").
        // We need to insert g.allocas between "entry:\n" and the body.
        // But the body was already emitted after "entry:\n".
        //
        // Let me restructure: don't emit "entry:" yet. Compile body into a temp buffer,
        // then emit entry: + allocas + body.
        //
        // I'll save the current buffer position, compile body, then rearrange.

        // If this is main, initialize top-level globals first
        if (strcmp(f->func.name, "main") == 0) {
            for (int gi = 0; gi < p->global_count; gi++) {
                Node *gvar = p->globals[gi];
                int gidx = find_global_ir(&g, gvar->var_decl.name);
                char *val = compile_expr_ir(&g, gvar->var_decl.value);
                ir_emit(&g, "  store %%Value %s, %%Value* %s, align 8\n", val, g.global_ptrs[gidx]);
                free(val);
            }
        }
        compile_stmt_ir(&g, f->func.body);

        // Ensure function ends with ret
        // Check if last instruction is ret
        // Simple: always append a ret nil (unreachable if body already returns)
        ir_emit(&g, "  ret %%Value { i64 0, i64 0 }\n");

        // Now insert allocas after "entry:\n"
        // g.buf has: ... "entry:\n" [body] [ret]
        // We need: ... "entry:\n" [allocas] [body] [ret]
        if (g.allocas_len > 0) {
            int body_len = g.len - entry_pos;
            // Ensure buffer has room for allocas + body + null terminator
            int needed = g.len + g.allocas_len + 1;
            if (needed > g.cap) {
                while (needed > g.cap) g.cap *= 2;
                g.buf = realloc(g.buf, g.cap);
            }
            char *body_copy = malloc(body_len);
            memcpy(body_copy, g.buf + entry_pos, body_len);
            ir_reserve(&g, g.allocas_len);
            // Insert allocas
            memcpy(g.buf + entry_pos, g.allocas, g.allocas_len);
            // Copy body after allocas
            memcpy(g.buf + entry_pos + g.allocas_len, body_copy, body_len);
            g.len += g.allocas_len;
            g.buf[g.len] = '\0';
            free(body_copy);
        }

        ir_emit(&g, "}\n\n");

        // Free var names
        for (int i = 0; i < g.var_count; i++) {
            free(g.var_names[i]);
            free(g.var_regs[i]);
        }
        free(g.var_names);
        free(g.var_regs);
    }

    // Main function
    ir_emit(&g, "define i32 @main(i32 %%argc, i8** %%argv) {\n");
    ir_emit(&g, "entry:\n");
    ir_emit(&g, "  call void @ml_set_args(i32 %%argc, i8** %%argv)\n");
    ir_emit(&g, "  %%result = call %%Value @user_main()\n");
    ir_emit(&g, "  %%exitcode = extractvalue %%Value %%result, 1\n");
    ir_emit(&g, "  ret i32 0\n");
    ir_emit(&g, "}\n");

    // Prepend string globals after the declare section (before first define)
    // Find the position of first "define" in the buffer
    char *first_define = strstr(g.buf, "\ndefine ");
    if (first_define && g.globals_len > 0) {
        int insert_pos = first_define - g.buf + 1; // +1 for the newline
        int tail_len = g.len - insert_pos;
        ir_reserve(&g, g.globals_len + 1);
        char *tail = malloc(tail_len);
        memcpy(tail, g.buf + insert_pos, tail_len);
        // Insert globals + newline
        memcpy(g.buf + insert_pos, g.globals, g.globals_len);
        g.buf[insert_pos + g.globals_len] = '\n';
        memcpy(g.buf + insert_pos + g.globals_len + 1, tail, tail_len);
        g.len += g.globals_len + 1;
        g.buf[g.len] = '\0';
        free(tail);
    }

    for (int i = 0; i < g.global_count; i++) {
        free(g.global_names[i]);
        free(g.global_ptrs[i]);
    }
    free(g.global_names);
    free(g.global_ptrs);
    for (int i = 0; i < g.break_count; i++) free(g.break_targets[i]);
    free(g.break_targets);
    free(g.globals);
    free(g.allocas);
    return g.buf;
}
