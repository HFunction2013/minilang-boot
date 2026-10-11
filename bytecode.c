#include "minilang.h"

Program *program_new(void) {
    Program *p = calloc(1, sizeof(Program));
    p->bc.code = NULL; p->bc.count = 0; p->bc.cap = 0;
    return p;
}

void program_free(Program *prog) {
    for (int i = 0; i < prog->const_count; i++) value_free(prog->constants[i]);
    free(prog->constants);
    for (int i = 0; i < prog->func_count; i++) free(prog->funcs[i].name);
    free(prog->funcs);
    free(prog->bc.code);
    free(prog);
}

int program_add_const(Program *prog, Value v) {
    // Deduplicate strings and ints
    for (int i = 0; i < prog->const_count; i++) {
        Value *c = &prog->constants[i];
        if (c->type == v.type) {
            if (c->type == VAL_INT && c->as.integer == v.as.integer) return i;
            if (c->type == VAL_STRING && strcmp(c->as.string, v.as.string) == 0) return i;
        }
    }
    if (prog->const_count >= prog->const_cap) {
        prog->const_cap = prog->const_cap ? prog->const_cap * 2 : 64;
        prog->constants = realloc(prog->constants, sizeof(Value) * prog->const_cap);
    }
    prog->constants[prog->const_count] = v;
    return prog->const_count++;
}

int program_add_func(Program *prog, const char *name, int address, int params) {
    if (prog->func_count >= prog->func_cap) {
        prog->func_cap = prog->func_cap ? prog->func_cap * 2 : 16;
        prog->funcs = realloc(prog->funcs, sizeof(FuncInfo) * prog->func_cap);
    }
    FuncInfo *f = &prog->funcs[prog->func_count];
    f->name = strdup(name);
    f->address = address;
    f->param_count = params;
    f->local_count = 0;
    f->is_native = 0; /* realloc may leave garbage in grown slots */
    return prog->func_count++;
}

void bc_emit(Program *prog, int op, int op1, int op2) {
    if (prog->bc.count + 3 > prog->bc.cap) {
        prog->bc.cap = prog->bc.cap ? prog->bc.cap * 2 : 256;
        prog->bc.code = realloc(prog->bc.code, sizeof(int) * prog->bc.cap);
    }
    prog->bc.code[prog->bc.count++] = op;
    prog->bc.code[prog->bc.count++] = op1;
    prog->bc.code[prog->bc.count++] = op2;
}

/* ===================== Compiler ===================== */

typedef struct {
    Program *prog;
    Parser *parser;
    // Current function variable tracking
    char **var_names;
    int var_count;
    int var_cap;
    int current_func;
    // Global variable names
    char **global_names;
    int global_count;
    // Break jump positions for current loop (stack of arrays)
    int **break_jumps;
    int *break_counts;
    int *break_caps;
    int loop_depth;
} Compiler;

static int find_var(Compiler *c, const char *name) {
    for (int i = 0; i < c->var_count; i++) {
        if (strcmp(c->var_names[i], name) == 0) return i;
    }
    return -1;
}

static int find_global(Compiler *c, const char *name) {
    for (int i = 0; i < c->global_count; i++) {
        if (strcmp(c->global_names[i], name) == 0) return i;
    }
    return -1;
}

static void loop_enter(Compiler *c) {
    if (c->loop_depth >= 64) { fprintf(stderr, "Compile error: loop nesting too deep\n"); exit(1); }
    c->break_jumps[c->loop_depth] = NULL;
    c->break_counts[c->loop_depth] = 0;
    c->break_caps[c->loop_depth] = 0;
    c->loop_depth++;
}

static void loop_add_break(Compiler *c, int jmp_pos) {
    if (c->loop_depth <= 0) { fprintf(stderr, "Compile error: break outside loop\n"); exit(1); }
    int d = c->loop_depth - 1;
    if (c->break_counts[d] >= c->break_caps[d]) {
        c->break_caps[d] = c->break_caps[d] ? c->break_caps[d] * 2 : 8;
        c->break_jumps[d] = realloc(c->break_jumps[d], sizeof(int) * c->break_caps[d]);
    }
    c->break_jumps[d][c->break_counts[d]++] = jmp_pos;
}

static void loop_exit(Compiler *c, int end_addr) {
    c->loop_depth--;
    int d = c->loop_depth;
    for (int i = 0; i < c->break_counts[d]; i++) {
        c->prog->bc.code[c->break_jumps[d][i] + 1] = end_addr;
    }
    free(c->break_jumps[d]);
    c->break_jumps[d] = NULL;
    c->break_counts[d] = 0;
}

static int add_var(Compiler *c, const char *name) {
    int existing = find_var(c, name);
    if (existing >= 0) return existing;
    if (c->var_count >= c->var_cap) {
        c->var_cap = c->var_cap ? c->var_cap * 2 : 16;
        c->var_names = realloc(c->var_names, sizeof(char*) * c->var_cap);
    }
    c->var_names[c->var_count] = strdup(name);
    return c->var_count++;
}

static int find_func(Compiler *c, const char *name) {
    for (int i = 0; i < c->prog->func_count; i++) {
        if (strcmp(c->prog->funcs[i].name, name) == 0) return i;
    }
    return -1;
}

static void compile_expr(Compiler *c, Node *n);

static void compile_stmt(Compiler *c, Node *n) {
    if (!n) return;
    switch (n->type) {
        case NODE_VAR_DECL: {
            int idx = add_var(c, n->var_decl.name);
            compile_expr(c, n->var_decl.value);
            bc_emit(c->prog, OP_STORE, idx, 0);
            break;
        }
        case NODE_ASSIGN: {
            int idx = find_var(c, n->assign.name);
            if (idx >= 0) {
                compile_expr(c, n->assign.value);
                bc_emit(c->prog, OP_STORE, idx, 0);
            } else {
                int gidx = find_global(c, n->assign.name);
                if (gidx < 0) {
                    fprintf(stderr, "Compile error: undefined variable '%s'\n", n->assign.name);
                    exit(1);
                }
                compile_expr(c, n->assign.value);
                bc_emit(c->prog, OP_STORE_GLOBAL, gidx, 0);
            }
            break;
        }
        case NODE_INDEX_ASSIGN: {
            compile_expr(c, n->index_assign.target); // array
            compile_expr(c, n->index_assign.index);  // index
            compile_expr(c, n->index_assign.value);  // value
            bc_emit(c->prog, OP_INDEX_SET, 0, 0);
            break;
        }
        case NODE_IF: {
            compile_expr(c, n->if_stmt.cond);
            int jmpf_pos = c->prog->bc.count;
            bc_emit(c->prog, OP_JMPF, 0, 0); // patch later
            compile_stmt(c, n->if_stmt.then_branch);
            if (n->if_stmt.else_branch) {
                int jmp_pos = c->prog->bc.count;
                bc_emit(c->prog, OP_JMP, 0, 0);
                int else_start = c->prog->bc.count / 3;
                c->prog->bc.code[jmpf_pos + 1] = else_start;
                compile_stmt(c, n->if_stmt.else_branch);
                int end = c->prog->bc.count / 3;
                c->prog->bc.code[jmp_pos + 1] = end;
            } else {
                int end = c->prog->bc.count / 3;
                c->prog->bc.code[jmpf_pos + 1] = end;
            }
            break;
        }
        case NODE_WHILE: {
            loop_enter(c);
            int start = c->prog->bc.count / 3;
            compile_expr(c, n->while_stmt.cond);
            int jmpf_pos = c->prog->bc.count;
            bc_emit(c->prog, OP_JMPF, 0, 0);
            compile_stmt(c, n->while_stmt.body);
            bc_emit(c->prog, OP_JMP, start, 0);
            int end = c->prog->bc.count / 3;
            c->prog->bc.code[jmpf_pos + 1] = end;
            loop_exit(c, end);
            break;
        }
        case NODE_RETURN: {
            if (n->return_stmt.value) {
                compile_expr(c, n->return_stmt.value);
            } else {
                bc_emit(c->prog, OP_NIL, 0, 0);
            }
            bc_emit(c->prog, OP_RET, 0, 0);
            break;
        }
        case NODE_PRINT: {
            compile_expr(c, n->print.value);
            if (n->print.newline) bc_emit(c->prog, OP_PRINTLN, 0, 0);
            else bc_emit(c->prog, OP_PRINT, 0, 0);
            break;
        }
        case NODE_BREAK: {
            int jmp_pos = c->prog->bc.count;
            bc_emit(c->prog, OP_JMP, 0, 0);
            loop_add_break(c, jmp_pos);
            break;
        }
        case NODE_BLOCK: {
            for (int i = 0; i < n->block.count; i++)
                compile_stmt(c, n->block.stmts[i]);
            break;
        }
        case NODE_EXPR_STMT: {
            compile_expr(c, n->expr_stmt.expr);
            bc_emit(c->prog, OP_POP, 0, 0);
            break;
        }
        default:
            fprintf(stderr, "Compile error: unexpected statement node type %d\n", n->type);
            exit(1);
    }
}

static void compile_expr(Compiler *c, Node *n) {
    switch (n->type) {
        case NODE_INT: {
            int idx = program_add_const(c->prog, make_int(n->integer.value));
            bc_emit(c->prog, OP_CONST, idx, 0);
            break;
        }
        case NODE_STRING: {
            int idx = program_add_const(c->prog, make_string(n->string.value));
            bc_emit(c->prog, OP_CONST, idx, 0);
            break;
        }
        case NODE_ARRAY_LIT: {
            for (int i = 0; i < n->array_lit.count; i++)
                compile_expr(c, n->array_lit.items[i]);
            bc_emit(c->prog, OP_ARRAY, n->array_lit.count, 0);
            break;
        }
        case NODE_VAR: {
            int idx = find_var(c, n->var.name);
            if (idx >= 0) {
                bc_emit(c->prog, OP_LOAD, idx, 0);
            } else {
                int gidx = find_global(c, n->var.name);
                if (gidx < 0) {
                    fprintf(stderr, "Compile error: undefined variable '%s'\n", n->var.name);
                    exit(1);
                }
                bc_emit(c->prog, OP_LOAD_GLOBAL, gidx, 0);
            }
            break;
        }
        case NODE_BINARY: {
            compile_expr(c, n->binary.left);
            compile_expr(c, n->binary.right);
            switch (n->binary.op) {
                case TOK_PLUS: bc_emit(c->prog, OP_ADD, 0, 0); break;
                case TOK_MINUS: bc_emit(c->prog, OP_SUB, 0, 0); break;
                case TOK_STAR: bc_emit(c->prog, OP_MUL, 0, 0); break;
                case TOK_SLASH: bc_emit(c->prog, OP_DIV, 0, 0); break;
                case TOK_PERCENT: bc_emit(c->prog, OP_MOD, 0, 0); break;
                case TOK_EQEQ: bc_emit(c->prog, OP_EQ, 0, 0); break;
                case TOK_NEQ: bc_emit(c->prog, OP_NEQ, 0, 0); break;
                case TOK_LT: bc_emit(c->prog, OP_LT, 0, 0); break;
                case TOK_GT: bc_emit(c->prog, OP_GT, 0, 0); break;
                case TOK_LTE: bc_emit(c->prog, OP_LTE, 0, 0); break;
                case TOK_GTE: bc_emit(c->prog, OP_GTE, 0, 0); break;
                case TOK_AND: bc_emit(c->prog, OP_AND, 0, 0); break;
                case TOK_OR: bc_emit(c->prog, OP_OR, 0, 0); break;
                default:
                    fprintf(stderr, "Compile error: unknown binary op %d\n", n->binary.op);
                    exit(1);
            }
            break;
        }
        case NODE_UNARY: {
            compile_expr(c, n->unary.operand);
            if (n->unary.op == TOK_MINUS) bc_emit(c->prog, OP_NEG, 0, 0);
            else if (n->unary.op == TOK_NOT) bc_emit(c->prog, OP_NOT, 0, 0);
            break;
        }
        case NODE_CALL: {
            // Built-in functions
            if (strcmp(n->call.name, "len") == 0) {
                compile_expr(c, n->call.args[0]);
                bc_emit(c->prog, OP_LEN, 0, 0);
                break;
            }
            if (strcmp(n->call.name, "charAt") == 0) {
                compile_expr(c, n->call.args[0]);
                compile_expr(c, n->call.args[1]);
                bc_emit(c->prog, OP_CHARAT, 0, 0);
                break;
            }
            if (strcmp(n->call.name, "substr") == 0) {
                compile_expr(c, n->call.args[0]);
                compile_expr(c, n->call.args[1]);
                compile_expr(c, n->call.args[2]);
                bc_emit(c->prog, OP_SUBSTR, 0, 0);
                break;
            }
            if (strcmp(n->call.name, "toString") == 0) {
                compile_expr(c, n->call.args[0]);
                bc_emit(c->prog, OP_TOSTRING, 0, 0);
                break;
            }
            if (strcmp(n->call.name, "toInt") == 0) {
                compile_expr(c, n->call.args[0]);
                bc_emit(c->prog, OP_TOINT, 0, 0);
                break;
            }
            if (strcmp(n->call.name, "strcmp") == 0) {
                compile_expr(c, n->call.args[0]);
                compile_expr(c, n->call.args[1]);
                bc_emit(c->prog, OP_STRCMP, 0, 0);
                break;
            }
            if (strcmp(n->call.name, "readAll") == 0) {
                bc_emit(c->prog, OP_READALL, 0, 0);
                break;
            }
            if (strcmp(n->call.name, "array") == 0) {
                compile_expr(c, n->call.args[0]); // size
                compile_expr(c, n->call.args[1]); // init
                bc_emit(c->prog, OP_MAKE_ARRAY, 0, 0);
                break;
            }
            if (strcmp(n->call.name, "argc") == 0) {
                bc_emit(c->prog, OP_ARGC, 0, 0);
                break;
            }
            if (strcmp(n->call.name, "argv") == 0) {
                compile_expr(c, n->call.args[0]);
                bc_emit(c->prog, OP_ARGV, 0, 0);
                break;
            }
            if (strcmp(n->call.name, "readFile") == 0) {
                compile_expr(c, n->call.args[0]);
                bc_emit(c->prog, OP_READFILE, 0, 0);
                break;
            }
            if (strcmp(n->call.name, "fileExists") == 0) {
                compile_expr(c, n->call.args[0]);
                bc_emit(c->prog, OP_FILEEXISTS, 0, 0);
                break;
            }
            if (strcmp(n->call.name, "writeFile") == 0) {
                compile_expr(c, n->call.args[0]); // path
                compile_expr(c, n->call.args[1]); // content
                bc_emit(c->prog, OP_WRITEFILE, 0, 0);
                break;
            }
            if (strcmp(n->call.name, "system") == 0) {
                compile_expr(c, n->call.args[0]);
                bc_emit(c->prog, OP_SYSTEM, 0, 0);
                break;
            }
            if (strcmp(n->call.name, "readLine") == 0) {
                bc_emit(c->prog, OP_READLINE, 0, 0);
                break;
            }
            if (strcmp(n->call.name, "readFileBytes") == 0) {
                compile_expr(c, n->call.args[0]);
                bc_emit(c->prog, OP_READFILEBYTES, 0, 0);
                break;
            }
            if (strcmp(n->call.name, "writeFileBytes") == 0) {
                compile_expr(c, n->call.args[0]); // path
                compile_expr(c, n->call.args[1]); // byte array
                bc_emit(c->prog, OP_WRITEFILEBYTES, 0, 0);
                break;
            }
            if (strcmp(n->call.name, "chr") == 0) {
                compile_expr(c, n->call.args[0]);
                bc_emit(c->prog, OP_CHR, 0, 0);
                break;
            }
            // User-defined function
            // Resolve alias: if n->call.name is an alias, use target name
            const char *cname = n->call.name;
            for (int ai = 0; ai < c->parser->alias_count; ai++) {
                if (strcmp(c->parser->alias_names[ai], cname) == 0) { cname = c->parser->alias_targets[ai]; break; }
            }
            int fi = find_func(c, cname);
            if (fi < 0) {
                fprintf(stderr, "Compile error: undefined function '%s'\n", cname);
                exit(1);
            }
            for (int i = 0; i < n->call.arg_count; i++)
                compile_expr(c, n->call.args[i]);
            if (c->prog->funcs[fi].is_native) {
                // Native (C-implemented) function: dispatch via VM registry.
                bc_emit(c->prog, OP_NATIVE, fi, n->call.arg_count);
            } else {
                bc_emit(c->prog, OP_CALL, fi, n->call.arg_count);
            }
            break;
        }
        case NODE_INDEX: {
            compile_expr(c, n->index.array);
            compile_expr(c, n->index.index);
            bc_emit(c->prog, OP_INDEX, 0, 0);
            break;
        }
        case NODE_NIL:
            bc_emit(c->prog, OP_NIL, 0, 0);
            break;
        default:
            fprintf(stderr, "Compile error: unexpected expression node type %d\n", n->type);
            exit(1);
    }
}

Program *compile_to_bytecode(Parser *p) {
    Compiler c;
    c.prog = program_new();
    c.parser = p;
    c.var_names = NULL;
    c.var_count = 0;
    c.var_cap = 0;
    c.current_func = -1;
    c.global_names = NULL;
    c.global_count = 0;
    c.break_jumps = calloc(64, sizeof(int*));
    c.break_counts = calloc(64, sizeof(int));
    c.break_caps = calloc(64, sizeof(int));
    c.loop_depth = 0;

    // Register global variable names
    for (int i = 0; i < p->global_count; i++) {
        Node *g = p->globals[i];
        if (c.global_count == 0) c.global_names = malloc(sizeof(char*));
        else c.global_names = realloc(c.global_names, sizeof(char*) * (c.global_count + 1));
        c.global_names[c.global_count++] = strdup(g->var_decl.name);

        if (c.prog->global_count >= c.prog->global_cap) {
            c.prog->global_cap = c.prog->global_cap ? c.prog->global_cap * 2 : 16;
            c.prog->global_names = realloc(c.prog->global_names, sizeof(char*) * c.prog->global_cap);
        }
        c.prog->global_names[c.prog->global_count] = strdup(g->var_decl.name);
        c.prog->global_count++;
    }

    // First pass: register all functions
    for (int i = 0; i < p->func_count; i++) {
        Node *f = p->funcs[i];
        program_add_func(c.prog, f->func.name, 0, f->func.param_count);
        if (f->func.is_native) {
            // Native functions have no bytecode body; the VM dispatches to C.
            c.prog->funcs[i].is_native = 1;
            c.prog->funcs[i].address = -1;
        }
    }

    int main_idx = find_func(&c, "main");
    if (main_idx < 0) {
        fprintf(stderr, "Compile error: no 'main' function found\n");
        exit(1);
    }

    // Emit jump to init section (will be patched)
    int jmp_init_pos = c.prog->bc.count;
    bc_emit(c.prog, OP_JMP, 0, 0);

    // Second pass: compile each function
    for (int i = 0; i < p->func_count; i++) {
        Node *f = p->funcs[i];
        c.var_names = NULL;
        c.var_count = 0;
        c.var_cap = 0;
        c.current_func = i;

        if (f->func.is_native) {
            // No body; nothing to emit. locals = param_count (declared but unused).
            c.prog->funcs[i].local_count = f->func.param_count;
            free(c.var_names);
            continue;
        }

        c.prog->funcs[i].address = c.prog->bc.count / 3;

        for (int j = 0; j < f->func.param_count; j++) {
            add_var(&c, f->func.params[j]);
        }

        compile_stmt(&c, f->func.body);
        bc_emit(c.prog, OP_NIL, 0, 0);
        bc_emit(c.prog, OP_RET, 0, 0);

        c.prog->funcs[i].local_count = c.var_count;

        for (int j = 0; j < c.var_count; j++) free(c.var_names[j]);
        free(c.var_names);
    }

    // Init section: initialize globals, call main, halt
    int init_addr = c.prog->bc.count / 3;
    c.prog->bc.code[jmp_init_pos + 1] = init_addr;

    for (int i = 0; i < p->global_count; i++) {
        Node *g = p->globals[i];
        c.var_names = NULL;
        c.var_count = 0;
        c.var_cap = 0;
        compile_expr(&c, g->var_decl.value);
        bc_emit(c.prog, OP_STORE_GLOBAL, i, 0);
        for (int j = 0; j < c.var_count; j++) free(c.var_names[j]);
        free(c.var_names);
    }

    bc_emit(c.prog, OP_CALL, main_idx, 0);
    bc_emit(c.prog, OP_HALT, 0, 0);

    // Free global names
    for (int i = 0; i < c.global_count; i++) free(c.global_names[i]);
    free(c.global_names);

    return c.prog;
}
