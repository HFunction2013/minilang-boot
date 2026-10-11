#include "minilang.h"

Node *node_new(NodeType type, int line) {
    Node *n = calloc(1, sizeof(Node));
    n->type = type;
    n->line = line;
    return n;
}

void node_free(Node *n) {
    if (!n) return;
    switch (n->type) {
        case NODE_STRING: free(n->string.value); break;
        case NODE_ARRAY_LIT:
            for (int i = 0; i < n->array_lit.count; i++) node_free(n->array_lit.items[i]);
            free(n->array_lit.items); break;
        case NODE_VAR: free(n->var.name); break;
        case NODE_VAR_DECL: free(n->var_decl.name); node_free(n->var_decl.value); break;
        case NODE_ASSIGN: free(n->assign.name); node_free(n->assign.value); break;
        case NODE_INDEX_ASSIGN:
            node_free(n->index_assign.target); node_free(n->index_assign.index);
            node_free(n->index_assign.value); break;
        case NODE_BINARY: node_free(n->binary.left); node_free(n->binary.right); break;
        case NODE_UNARY: node_free(n->unary.operand); break;
        case NODE_IF: node_free(n->if_stmt.cond); node_free(n->if_stmt.then_branch);
            node_free(n->if_stmt.else_branch); break;
        case NODE_WHILE: node_free(n->while_stmt.cond); node_free(n->while_stmt.body); break;
        case NODE_RETURN: node_free(n->return_stmt.value); break;
        case NODE_FUNC:
            free(n->func.name);
            for (int i = 0; i < n->func.param_count; i++) free(n->func.params[i]);
            free(n->func.params); node_free(n->func.body); break;
        case NODE_CALL:
            free(n->call.name);
            for (int i = 0; i < n->call.arg_count; i++) node_free(n->call.args[i]);
            free(n->call.args); break;
        case NODE_INDEX: node_free(n->index.array); node_free(n->index.index); break;
        case NODE_PRINT: node_free(n->print.value); break;
        case NODE_BLOCK:
            for (int i = 0; i < n->block.count; i++) node_free(n->block.stmts[i]);
            free(n->block.stmts); break;
        case NODE_EXPR_STMT: node_free(n->expr_stmt.expr); break;
        default: break;
    }
    free(n);
}

Parser *parser_new(Lexer *l) {
    Parser *p = calloc(1, sizeof(Parser));
    p->lexer = l; p->pos = 0;
    p->funcs = NULL; p->func_count = 0; p->func_cap = 0;
    p->globals = NULL; p->global_count = 0; p->global_cap = 0;
    p->modules = NULL; p->module_count = 0; p->module_cap = 0;
    p->alias_names = NULL; p->alias_targets = NULL; p->alias_count = 0; p->alias_cap = 0;
    return p;
}

void parser_free(Parser *p) {
    for (int i = 0; i < p->func_count; i++) node_free(p->funcs[i]);
    free(p->funcs);
    for (int i = 0; i < p->global_count; i++) node_free(p->globals[i]);
    free(p->globals);
    for (int i = 0; i < p->module_count; i++) {
        free(p->modules[i].module_name);
        for (int j = 0; j < p->modules[i].export_count; j++) free(p->modules[i].exports[j]);
        free(p->modules[i].exports);
    }
    free(p->modules);
    for (int i = 0; i < p->alias_count; i++) {
        free(p->alias_names[i]);
        free(p->alias_targets[i]);
    }
    free(p->alias_names);
    free(p->alias_targets);
    free(p);
}

static Token *cur(Parser *p) { return &p->lexer->tokens[p->pos]; }
static Token *peek_tok(Parser *p, int o) { return &p->lexer->tokens[p->pos + o]; }
static Token *advance_p(Parser *p) { return &p->lexer->tokens[p->pos++]; }
static int check(Parser *p, TokenType t) { return cur(p)->type == t; }
static Token *expect(Parser *p, TokenType t, const char *msg) {
    if (!check(p, t)) {
        fprintf(stderr, "Parse error line %d: expected %s, got type %d", cur(p)->line, msg, cur(p)->type);
        if (cur(p)->text) fprintf(stderr, " ('%s')", cur(p)->text);
        fprintf(stderr, "\n"); exit(1);
    }
    return advance_p(p);
}

static Node *parse_expr(Parser *p);
static Node *parse_stmt(Parser *p);
static Node *parse_block(Parser *p);
static Node *parse_postfix(Parser *p);

static Node *parse_primary(Parser *p) {
    Token *t = cur(p);
    if (t->type == TOK_INT) {
        advance_p(p);
        Node *n = node_new(NODE_INT, t->line);
        n->integer.value = t->intval; return n;
    }
    if (t->type == TOK_STRING) {
        advance_p(p);
        Node *n = node_new(NODE_STRING, t->line);
        n->string.value = strdup(t->text); return n;
    }
    if (t->type == TOK_LPAREN) {
        advance_p(p);
        Node *e = parse_expr(p);
        expect(p, TOK_RPAREN, "')'"); return e;
    }
    if (t->type == TOK_LBRACKET) {
        advance_p(p);
        Node **items = NULL; int count = 0, cap = 0;
        if (!check(p, TOK_RBRACKET)) {
            while (1) {
                Node *e = parse_expr(p);
                if (count >= cap) { cap = cap ? cap*2 : 4; items = realloc(items, sizeof(Node*)*cap); }
                items[count++] = e;
                if (check(p, TOK_COMMA)) { advance_p(p); continue; }
                break;
            }
        }
        expect(p, TOK_RBRACKET, "']'");
        Node *n = node_new(NODE_ARRAY_LIT, t->line);
        n->array_lit.items = items; n->array_lit.count = count; return n;
    }
    if (t->type == TOK_IDENT) {
        advance_p(p);
        // Namespace access: module.func(...)
        if (check(p, TOK_DOT)) {
            Token *module_tok = t;
            advance_p(p); // consume '.'
            Token *member = expect(p, TOK_IDENT, "function name after '.'");
            // Check that module_tok is a loaded module
            int mod_idx = -1;
            for (int i = 0; i < p->module_count; i++) {
                if (strcmp(p->modules[i].module_name, module_tok->text) == 0) { mod_idx = i; break; }
            }
            if (mod_idx < 0) {
                fprintf(stderr, "Parse error line %d: unknown module '%s'\n", module_tok->line, module_tok->text);
                exit(1);
            }
            // Build the full name "module.func"
            int len = strlen(module_tok->text) + strlen(member->text) + 2;
            char *full = malloc(len);
            snprintf(full, len, "%s.%s", module_tok->text, member->text);
            if (check(p, TOK_LPAREN)) {
                advance_p(p);
                Node **args = NULL; int argc = 0, acap = 0;
                if (!check(p, TOK_RPAREN)) {
                    while (1) {
                        Node *a = parse_expr(p);
                        if (argc >= acap) { acap = acap ? acap*2 : 4; args = realloc(args, sizeof(Node*)*acap); }
                        args[argc++] = a;
                        if (check(p, TOK_COMMA)) { advance_p(p); continue; }
                        break;
                    }
                }
                expect(p, TOK_RPAREN, "')'");
                Node *n = node_new(NODE_CALL, t->line);
                n->call.name = full; // "math.cos"
                n->call.args = args; n->call.arg_count = argc; return n;
            }
            Node *n = node_new(NODE_VAR, t->line);
            n->var.name = full; return n;
        }
        if (check(p, TOK_LPAREN)) {
            advance_p(p);
            Node **args = NULL; int argc = 0, acap = 0;
            if (!check(p, TOK_RPAREN)) {
                while (1) {
                    Node *a = parse_expr(p);
                    if (argc >= acap) { acap = acap ? acap*2 : 4; args = realloc(args, sizeof(Node*)*acap); }
                    args[argc++] = a;
                    if (check(p, TOK_COMMA)) { advance_p(p); continue; }
                    break;
                }
            }
            expect(p, TOK_RPAREN, "')'");
            Node *n = node_new(NODE_CALL, t->line);
            n->call.name = strdup(t->text);
            n->call.args = args; n->call.arg_count = argc; return n;
        }
        Node *n = node_new(NODE_VAR, t->line);
        n->var.name = strdup(t->text); return n;
    }
    fprintf(stderr, "Parse error line %d: unexpected token", t->line);
    if (t->text) fprintf(stderr, " '%s'", t->text);
    fprintf(stderr, "\n"); exit(1);
}

static Node *parse_postfix(Parser *p) {
    Node *left = parse_primary(p);
    while (check(p, TOK_LBRACKET)) {
        advance_p(p);
        Node *idx = parse_expr(p);
        expect(p, TOK_RBRACKET, "']'");
        Node *n = node_new(NODE_INDEX, left->line);
        n->index.array = left; n->index.index = idx;
        left = n;
    }
    return left;
}

static Node *parse_unary(Parser *p) {
    if (check(p, TOK_MINUS) || check(p, TOK_NOT)) {
        Token *t = advance_p(p);
        Node *operand = parse_unary(p);
        Node *n = node_new(NODE_UNARY, t->line);
        n->unary.operand = operand; n->unary.op = t->type; return n;
    }
    return parse_postfix(p);
}

static Node *parse_mul(Parser *p) {
    Node *left = parse_unary(p);
    while (check(p, TOK_STAR) || check(p, TOK_SLASH) || check(p, TOK_PERCENT)) {
        Token *t = advance_p(p);
        Node *right = parse_unary(p);
        Node *n = node_new(NODE_BINARY, t->line);
        n->binary.left = left; n->binary.right = right; n->binary.op = t->type;
        left = n;
    }
    return left;
}

static Node *parse_add(Parser *p) {
    Node *left = parse_mul(p);
    while (check(p, TOK_PLUS) || check(p, TOK_MINUS)) {
        Token *t = advance_p(p);
        Node *right = parse_mul(p);
        Node *n = node_new(NODE_BINARY, t->line);
        n->binary.left = left; n->binary.right = right; n->binary.op = t->type;
        left = n;
    }
    return left;
}

static Node *parse_comparison(Parser *p) {
    Node *left = parse_add(p);
    while (check(p, TOK_LT) || check(p, TOK_GT) || check(p, TOK_LTE) || check(p, TOK_GTE)) {
        Token *t = advance_p(p);
        Node *right = parse_add(p);
        Node *n = node_new(NODE_BINARY, t->line);
        n->binary.left = left; n->binary.right = right; n->binary.op = t->type;
        left = n;
    }
    return left;
}

static Node *parse_equality(Parser *p) {
    Node *left = parse_comparison(p);
    while (check(p, TOK_EQEQ) || check(p, TOK_NEQ)) {
        Token *t = advance_p(p);
        Node *right = parse_comparison(p);
        Node *n = node_new(NODE_BINARY, t->line);
        n->binary.left = left; n->binary.right = right; n->binary.op = t->type;
        left = n;
    }
    return left;
}

static Node *parse_and(Parser *p) {
    Node *left = parse_equality(p);
    while (check(p, TOK_AND)) {
        Token *t = advance_p(p);
        Node *right = parse_equality(p);
        Node *n = node_new(NODE_BINARY, t->line);
        n->binary.left = left; n->binary.right = right; n->binary.op = t->type;
        left = n;
    }
    return left;
}

static Node *parse_or(Parser *p) {
    Node *left = parse_and(p);
    while (check(p, TOK_OR)) {
        Token *t = advance_p(p);
        Node *right = parse_and(p);
        Node *n = node_new(NODE_BINARY, t->line);
        n->binary.left = left; n->binary.right = right; n->binary.op = t->type;
        left = n;
    }
    return left;
}

static Node *parse_expr(Parser *p) { return parse_or(p); }

static Node *parse_block(Parser *p) {
    expect(p, TOK_LBRACE, "'{'");
    Node **stmts = NULL; int count = 0, cap = 0;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Node *s = parse_stmt(p);
        if (count >= cap) { cap = cap ? cap*2 : 8; stmts = realloc(stmts, sizeof(Node*)*cap); }
        stmts[count++] = s;
    }
    expect(p, TOK_RBRACE, "'}'");
    Node *n = node_new(NODE_BLOCK, 0);
    n->block.stmts = stmts; n->block.count = count; return n;
}

static Node *parse_stmt(Parser *p) {
    Token *t = cur(p);

    if (t->type == TOK_VAR) {
        advance_p(p);
        Token *name = expect(p, TOK_IDENT, "identifier after 'var'");
        expect(p, TOK_EQ, "'=' in var");
        Node *val = parse_expr(p);
        expect(p, TOK_SEMICOLON, "';'");
        Node *n = node_new(NODE_VAR_DECL, t->line);
        n->var_decl.name = strdup(name->text); n->var_decl.value = val; return n;
    }
    if (t->type == TOK_IF) {
        advance_p(p);
        expect(p, TOK_LPAREN, "'('");
        Node *cond = parse_expr(p);
        expect(p, TOK_RPAREN, "')'");
        Node *then_b = parse_stmt(p);
        Node *else_b = NULL;
        if (check(p, TOK_ELSE)) { advance_p(p); else_b = parse_stmt(p); }
        Node *n = node_new(NODE_IF, t->line);
        n->if_stmt.cond = cond; n->if_stmt.then_branch = then_b; n->if_stmt.else_branch = else_b;
        return n;
    }
    if (t->type == TOK_WHILE) {
        advance_p(p);
        expect(p, TOK_LPAREN, "'('");
        Node *cond = parse_expr(p);
        expect(p, TOK_RPAREN, "')'");
        Node *body = parse_stmt(p);
        Node *n = node_new(NODE_WHILE, t->line);
        n->while_stmt.cond = cond; n->while_stmt.body = body; return n;
    }
    if (t->type == TOK_RETURN) {
        advance_p(p);
        Node *val = NULL;
        if (!check(p, TOK_SEMICOLON)) val = parse_expr(p);
        expect(p, TOK_SEMICOLON, "';'");
        Node *n = node_new(NODE_RETURN, t->line);
        n->return_stmt.value = val; return n;
    }
    if (t->type == TOK_PRINT || t->type == TOK_PRINTLN) {
        advance_p(p);
        Node *val = parse_expr(p);
        expect(p, TOK_SEMICOLON, "';'");
        Node *n = node_new(NODE_PRINT, t->line);
        n->print.value = val; n->print.newline = (t->type == TOK_PRINTLN); return n;
    }
    if (t->type == TOK_BREAK) {
        advance_p(p);
        expect(p, TOK_SEMICOLON, "';'");
        return node_new(NODE_BREAK, t->line);
    }
    if (t->type == TOK_LBRACE) return parse_block(p);

    // Assignment: IDENT '='  (lookahead, no consume)
    if (t->type == TOK_IDENT && peek_tok(p, 1)->type == TOK_EQ) {
        Token *name = advance_p(p);
        advance_p(p); // '='
        Node *val = parse_expr(p);
        expect(p, TOK_SEMICOLON, "';'");
        Node *n = node_new(NODE_ASSIGN, t->line);
        n->assign.name = strdup(name->text); n->assign.value = val; return n;
    }
    // Index assignment: IDENT '[' ... ']' '='
    if (t->type == TOK_IDENT && peek_tok(p, 1)->type == TOK_LBRACKET) {
        Node *left = parse_postfix(p);
        if (left->type == NODE_INDEX && check(p, TOK_EQ)) {
            advance_p(p);
            Node *val = parse_expr(p);
            expect(p, TOK_SEMICOLON, "';'");
            Node *n = node_new(NODE_INDEX_ASSIGN, t->line);
            n->index_assign.target = left->index.array;
            n->index_assign.index = left->index.index;
            n->index_assign.value = val;
            free(left); return n;
        }
        // Expression statement starting with index access
        expect(p, TOK_SEMICOLON, "';'");
        Node *n = node_new(NODE_EXPR_STMT, t->line);
        n->expr_stmt.expr = left; return n;
    }

    // Expression statement
    Node *e = parse_expr(p);
    expect(p, TOK_SEMICOLON, "';'");
    Node *n = node_new(NODE_EXPR_STMT, t->line);
    n->expr_stmt.expr = e; return n;
}

static Node *parse_func(Parser *p, int is_native) {
    Token *t = expect(p, TOK_FUNC, "'func'");
    Token *name = expect(p, TOK_IDENT, "function name");
    expect(p, TOK_LPAREN, "'('");
    char **params = NULL; int pcount = 0, pcap = 0;
    if (!check(p, TOK_RPAREN)) {
        while (1) {
            Token *pn = expect(p, TOK_IDENT, "param name");
            if (pcount >= pcap) { pcap = pcap ? pcap*2 : 4; params = realloc(params, sizeof(char*)*pcap); }
            params[pcount++] = strdup(pn->text);
            if (check(p, TOK_COMMA)) { advance_p(p); continue; }
            break;
        }
    }
    expect(p, TOK_RPAREN, "')'");
    Node *body;
    if (is_native) {
        // `native func name(params);` — no body, implemented in C.
        expect(p, TOK_SEMICOLON, "';' after native function declaration");
        body = NULL;
    } else {
        body = parse_block(p);
    }
    Node *n = node_new(NODE_FUNC, t->line);
    n->func.name = strdup(name->text);
    n->func.params = params; n->func.param_count = pcount;
    n->func.body = body; n->func.is_native = is_native;
    return n;
}

/* ===================== require module system ===================== */
static char g_script_dir[1024] = ".";
static char g_syslib_dir[1024] = "syslib";
void parser_set_search_dirs(const char *script_dir, const char *syslib_dir) {
    if (script_dir) { snprintf(g_script_dir, sizeof(g_script_dir), "%s", script_dir); }
    if (syslib_dir) { snprintf(g_syslib_dir, sizeof(g_syslib_dir), "%s", syslib_dir); }
}
static void add_module_func(Parser *p, Node *f) {
    if (p->func_count >= p->func_cap) {
        p->func_cap = p->func_cap ? p->func_cap*2 : 8;
        p->funcs = realloc(p->funcs, sizeof(Node*)*p->func_cap);
    }
    p->funcs[p->func_count++] = f;
}
static int add_alias(Parser *p, const char *name, const char *target) {
    // Check for duplicate alias
    for (int i = 0; i < p->alias_count; i++) {
        if (strcmp(p->alias_names[i], name) == 0) return 0; // already imported
    }
    if (p->alias_count >= p->alias_cap) {
        p->alias_cap = p->alias_cap ? p->alias_cap*2 : 8;
        p->alias_names = realloc(p->alias_names, sizeof(char*)*p->alias_cap);
        p->alias_targets = realloc(p->alias_targets, sizeof(char*)*p->alias_cap);
    }
    p->alias_names[p->alias_count] = strdup(name);
    p->alias_targets[p->alias_count] = strdup(target);
    p->alias_count++;
    return 1;
}
static int module_already_loaded(Parser *p, const char *module) {
    for (int i = 0; i < p->module_count; i++) {
        if (strcmp(p->modules[i].module_name, module) == 0) return 1;
    }
    return 0;
}
/* Rename all NODE_CALL names in module funcs by adding module prefix for
   internal cross-references (function calls to other module functions).
   Local variables keep their names. */
static void rename_module_refs(Node *n, const char *prefix, int prefix_len) {
    if (!n) return;
    switch (n->type) {
        case NODE_FUNC: {
            // Rename calls inside body only; params stay as-is
            rename_module_refs(n->func.body, prefix, prefix_len);
            break;
        }
        case NODE_VAR_DECL:
            rename_module_refs(n->var_decl.value, prefix, prefix_len);
            break;
        case NODE_ASSIGN:
            rename_module_refs(n->assign.value, prefix, prefix_len);
            break;
        case NODE_BINARY:
            rename_module_refs(n->binary.left, prefix, prefix_len);
            rename_module_refs(n->binary.right, prefix, prefix_len);
            break;
        case NODE_UNARY:
            rename_module_refs(n->unary.operand, prefix, prefix_len);
            break;
        case NODE_IF:
            rename_module_refs(n->if_stmt.cond, prefix, prefix_len);
            rename_module_refs(n->if_stmt.then_branch, prefix, prefix_len);
            rename_module_refs(n->if_stmt.else_branch, prefix, prefix_len);
            break;
        case NODE_WHILE:
            rename_module_refs(n->while_stmt.cond, prefix, prefix_len);
            rename_module_refs(n->while_stmt.body, prefix, prefix_len);
            break;
        case NODE_RETURN:
            rename_module_refs(n->return_stmt.value, prefix, prefix_len);
            break;
        case NODE_CALL: {
            // Rename internal function calls (not builtins, not already qualified)
            if (strchr(n->call.name, '.') == NULL) {
                const char *builtins[] = {"len","charAt","substr","toString","toInt","strcmp","readAll","array","print","println","argc","argv","readFile","fileExists","writeFile","system","readLine","readFileBytes","writeFileBytes","chr"};
                int is_builtin = 0;
                for (size_t i = 0; i < sizeof(builtins)/sizeof(builtins[0]); i++) if (strcmp(n->call.name, builtins[i]) == 0) { is_builtin = 1; break; }
                if (!is_builtin) {
                    char *newname = malloc(strlen(n->call.name) + prefix_len + 1);
                    snprintf(newname, strlen(n->call.name) + prefix_len + 1, "%s%s", prefix, n->call.name);
                    free(n->call.name); n->call.name = newname;
                }
            }
            for (int i = 0; i < n->call.arg_count; i++) rename_module_refs(n->call.args[i], prefix, prefix_len);
            break;
        }
        case NODE_VAR:
            // keep as-is (local var or module global referenced by name)
            break;
        case NODE_INDEX:
            rename_module_refs(n->index.array, prefix, prefix_len);
            rename_module_refs(n->index.index, prefix, prefix_len);
            break;
        case NODE_INDEX_ASSIGN:
            rename_module_refs(n->index_assign.target, prefix, prefix_len);
            rename_module_refs(n->index_assign.index, prefix, prefix_len);
            rename_module_refs(n->index_assign.value, prefix, prefix_len);
            break;
        case NODE_PRINT:
            rename_module_refs(n->print.value, prefix, prefix_len);
            break;
        case NODE_BLOCK:
            for (int i = 0; i < n->block.count; i++) rename_module_refs(n->block.stmts[i], prefix, prefix_len);
            break;
        case NODE_EXPR_STMT:
            rename_module_refs(n->expr_stmt.expr, prefix, prefix_len);
            break;
        case NODE_ARRAY_LIT:
            for (int i = 0; i < n->array_lit.count; i++) rename_module_refs(n->array_lit.items[i], prefix, prefix_len);
            break;
        default: break;
    }
}
/* Parse a module file and merge its functions into the main parser. */
static void load_module(Parser *p, const char *module_name, int search_syslib_only) {
    // Build candidate paths: script dir first, then syslib
    char path[2048];
    const char *dirs[2];
    int ndirs;
    if (search_syslib_only == 1) {
        dirs[0] = g_syslib_dir; ndirs = 1;
    } else if (search_syslib_only == 2) { // cwd only
        dirs[0] = "."; ndirs = 1;
    } else {
        dirs[0] = g_script_dir; dirs[1] = g_syslib_dir; ndirs = 2;
    }
    char *src = NULL;
    char found_path[2048];
    for (int i = 0; i < ndirs; i++) {
        snprintf(path, sizeof(path), "%s/%s.mil", dirs[i], module_name);
        FILE *f = fopen(path, "rb");
        if (f) {
            fclose(f);
            char *s = read_file(path);
            if (s) { src = s; snprintf(found_path, sizeof(found_path), "%s", path); break; }
        }
    }
    if (!src) {
        fprintf(stderr, "Parse error: cannot find module '%s' (searched %s)\n", module_name, dirs[0]);
        exit(1);
    }
    // Lex and parse module source
    Lexer *lexer = lexer_new(src);
    lex(lexer);
    Parser *mp = parser_new(lexer);
    /* Propagate already-loaded modules from parent parser so nested
       require of the same module is skipped (avoids duplicate definitions). */
    for (int mi = 0; mi < p->module_count; mi++) {
        if (mp->module_count >= mp->module_cap) {
            mp->module_cap = mp->module_cap ? mp->module_cap*2 : 4;
            mp->modules = realloc(mp->modules, sizeof(ModuleInfo)*mp->module_cap);
        }
        ModuleInfo *mmi = &mp->modules[mp->module_count++];
        mmi->module_name = strdup(p->modules[mi].module_name);
        mmi->exports = NULL; mmi->export_count = 0; mmi->export_cap = 0;
    }
    /* Save/restore search dirs: modules resolve their own requires relative to same dirs */
    parse(mp);
    /* Rename module functions: module_name prefix */
    char prefix[1024];
    snprintf(prefix, sizeof(prefix), "%s.", module_name);
    int prefix_len = strlen(prefix);
    // Rename function definitions themselves (only unprefixed = this module's own funcs)
    for (int i = 0; i < mp->func_count; i++) {
        Node *f = mp->funcs[i];
        if (strchr(f->func.name, '.') != NULL) {
            // already qualified (nested require pulled it in): keep name, still rename
            // references inside its body so it can find this module's funcs
            rename_module_refs(f->func.body, prefix, prefix_len);
            continue;
        }
        char *newname = malloc(strlen(f->func.name) + prefix_len + 1);
        snprintf(newname, strlen(f->func.name) + prefix_len + 1, "%s%s", prefix, f->func.name);
        free(f->func.name); f->func.name = newname;
        // Rename internal references inside body
        rename_module_refs(f->func.body, prefix, prefix_len);
    }
    // Merge into main parser
    for (int i = 0; i < mp->func_count; i++) add_module_func(p, mp->funcs[i]);
    mp->func_count = 0;
    for (int i = 0; i < mp->global_count; i++) {
        Node *g = mp->globals[i];
        // Deduplicate: skip if a global with the same name already exists
        int exists = 0;
        for (int j = 0; j < p->global_count; j++) {
            if (strcmp(p->globals[j]->var_decl.name, g->var_decl.name) == 0) { exists = 1; break; }
        }
        if (exists) continue;
        if (p->global_count >= p->global_cap) {
            p->global_cap = p->global_cap ? p->global_cap*2 : 16;
            p->globals = realloc(p->globals, sizeof(Node*)*p->global_cap);
        }
        p->globals[p->global_count++] = g;
    }
    mp->global_count = 0;
    // Record module info with exports (function names)
    if (p->module_count >= p->module_cap) {
        p->module_cap = p->module_cap ? p->module_cap*2 : 4;
        p->modules = realloc(p->modules, sizeof(ModuleInfo)*p->module_cap);
    }
    ModuleInfo *mi = &p->modules[p->module_count++];
    mi->module_name = strdup(module_name);
    mi->exports = NULL; mi->export_count = 0; mi->export_cap = 0;
    // Exports: function names WITH prefix
    for (int i = 0; i < p->func_count; i++) {
        /* count funcs belonging to this module */
    }
    // Record export names (already prefixed) for namespace lookup
    for (int i = 0; i < p->func_count; i++) {
        char *fn = p->funcs[i]->func.name;
        if (strncmp(fn, prefix, prefix_len) == 0) {
            if (mi->export_count >= mi->export_cap) {
                mi->export_cap = mi->export_cap ? mi->export_cap*2 : 8;
                mi->exports = realloc(mi->exports, sizeof(char*)*mi->export_cap);
            }
            mi->exports[mi->export_count++] = strdup(fn);
        }
    }
    parser_free(mp);
    free(lexer);
    free(src);
}
/* Parse a require statement: require A; / require x from A; / require x,y from A; / ... in cwd|syslib */
static void parse_require(Parser *p) {
    advance_p(p); // consume 'require'
    // Collect imported names (if any)
    char **names = NULL; int ncount = 0, ncap = 0;
    char *module = NULL;
    if (check(p, TOK_IDENT) && (peek_tok(p, 1)->type == TOK_FROM || peek_tok(p, 1)->type == TOK_COMMA)) {
        // require x from A; or require x, y from A;
        while (1) {
            Token *nm = expect(p, TOK_IDENT, "identifier");
            if (ncount >= ncap) { ncap = ncap ? ncap*2 : 4; names = realloc(names, sizeof(char*)*ncap); }
            names[ncount++] = strdup(nm->text);
            if (check(p, TOK_COMMA)) { advance_p(p); continue; }
            break;
        }
        expect(p, TOK_FROM, "'from'");
        Token *mod = expect(p, TOK_IDENT, "module name");
        module = strdup(mod->text);
    } else {
        // require A;
        Token *mod = expect(p, TOK_IDENT, "module name");
        module = strdup(mod->text);
    }
    int search_syslib_only = 0; // 0=default, 1=syslib only, 2=cwd only
    if (check(p, TOK_IN)) {
        advance_p(p);
        Token *loc = expect(p, TOK_IDENT, "'syslib' or 'cwd'");
        if (strcmp(loc->text, "syslib") == 0) search_syslib_only = 1;
        else if (strcmp(loc->text, "cwd") == 0) search_syslib_only = 2;
        else { fprintf(stderr, "Parse error line %d: expected 'syslib' or 'cwd' after 'in'\n", loc->line); exit(1); }
    }
    expect(p, TOK_SEMICOLON, "';'");
    // Load module (if not already loaded)
    if (!module_already_loaded(p, module)) {
        load_module(p, module, search_syslib_only);
    }
    if (ncount > 0) {
        // require cos from math: alias cos -> math.cos
        for (int i = 0; i < ncount; i++) {
            char target[2048];
            snprintf(target, sizeof(target), "%s.%s", module, names[i]);
            // Verify the function exists in module
            int found = 0;
            for (int m = 0; m < p->module_count; m++) {
                if (strcmp(p->modules[m].module_name, module) == 0) {
                    for (int e = 0; e < p->modules[m].export_count; e++) {
                        if (strcmp(p->modules[m].exports[e], target) == 0) { found = 1; break; }
                    }
                }
            }
            if (!found) {
                fprintf(stderr, "Parse error: module '%s' has no exported function '%s'\n", module, names[i]);
                exit(1);
            }
            add_alias(p, names[i], target);
        }
    }
    for (int i = 0; i < ncount; i++) free(names[i]);
    free(names);
    free(module);
}
void parse(Parser *p) {
    while (!check(p, TOK_EOF)) {
        if (check(p, TOK_REQUIRE)) {
            parse_require(p);
        } else if (check(p, TOK_NATIVE)) {
            advance_p(p); // consume 'native'
            Node *f = parse_func(p, 1);
            if (p->func_count >= p->func_cap) {
                p->func_cap = p->func_cap ? p->func_cap*2 : 8;
                p->funcs = realloc(p->funcs, sizeof(Node*)*p->func_cap);
            }
            p->funcs[p->func_count++] = f;
        } else if (check(p, TOK_FUNC)) {
            Node *f = parse_func(p, 0);
            if (p->func_count >= p->func_cap) {
                p->func_cap = p->func_cap ? p->func_cap*2 : 8;
                p->funcs = realloc(p->funcs, sizeof(Node*)*p->func_cap);
            }
            p->funcs[p->func_count++] = f;
        } else if (check(p, TOK_VAR)) {
            // Top-level var declaration
            advance_p(p);
            Token *name = expect(p, TOK_IDENT, "identifier after 'var'");
            expect(p, TOK_EQ, "'=' in var");
            Node *val = parse_expr(p);
            expect(p, TOK_SEMICOLON, "';'");
            Node *n = node_new(NODE_VAR_DECL, name->line);
            n->var_decl.name = strdup(name->text);
            n->var_decl.value = val;
            if (p->global_count >= p->global_cap) {
                p->global_cap = p->global_cap ? p->global_cap*2 : 16;
                p->globals = realloc(p->globals, sizeof(Node*)*p->global_cap);
            }
            p->globals[p->global_count++] = n;
        } else {
            fprintf(stderr, "Parse error line %d: expected 'func' or 'var' at top level\n", cur(p)->line);
            exit(1);
        }
    }
}
