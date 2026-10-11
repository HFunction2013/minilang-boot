#ifndef MINILANG_H
#define MINILANG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>

/* ===================== Value types ===================== */
typedef enum {
    VAL_NIL,
    VAL_INT,
    VAL_STRING,
    VAL_ARRAY,
} ValueType;

typedef struct Value Value;
typedef struct {
    Value *items;
    int count;
    int cap;
} Array;

struct Value {
    ValueType type;
    union {
        int64_t integer;
        char *string;
        Array array;
    } as;
};

Value make_nil(void);
Value make_int(int64_t v);
Value make_string(const char *s);
Value make_string_n(const char *s, int n);
Value make_array(Value *items, int count);
void value_free(Value v);
void value_print(Value v, int newline);
int value_is_truthy(Value v);
Value value_add(Value a, Value b);
Value value_sub(Value a, Value b);
Value value_mul(Value a, Value b);
Value value_div(Value a, Value b);
Value value_mod(Value a, Value b);
Value value_neg(Value a);
Value value_eq(Value a, Value b);
Value value_neq(Value a, Value b);
Value value_lt(Value a, Value b);
Value value_gt(Value a, Value b);
Value value_lte(Value a, Value b);
Value value_gte(Value a, Value b);
Value value_and(Value a, Value b);
Value value_or(Value a, Value b);
Value value_not(Value a);
Value value_len(Value v);
Value value_charat(Value s, Value idx);
Value value_substr(Value s, Value start, Value len);
Value value_tostring(Value v);
Value value_toint(Value v);
Value value_strcmp(Value a, Value b);

/* ===================== Lexer ===================== */
typedef enum {
    TOK_EOF,
    TOK_INT,
    TOK_STRING,
    TOK_IDENT,
    // keywords
    TOK_VAR, TOK_FUNC, TOK_IF, TOK_ELSE, TOK_WHILE,
    TOK_RETURN, TOK_PRINT, TOK_PRINTLN, TOK_BREAK,
    TOK_REQUIRE, TOK_FROM, TOK_IN,
    TOK_NATIVE,
    // operators
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_EQ, TOK_EQEQ, TOK_NEQ, TOK_LT, TOK_GT, TOK_LTE, TOK_GTE,
    TOK_AND, TOK_OR, TOK_NOT, TOK_DOT,
    // punctuation
    TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET,
    TOK_COMMA, TOK_SEMICOLON,
} TokenType;

typedef struct {
    TokenType type;
    char *text;      // for ident/string: owned copy
    int64_t intval;  // for int literal
    int line;
} Token;

typedef struct {
    const char *src;
    int pos;
    int len;
    int line;
    Token *tokens;
    int count;
    int cap;
} Lexer;

Lexer *lexer_new(const char *src);
void lexer_free(Lexer *l);
void lex(Lexer *l);

/* ===================== AST ===================== */
typedef enum {
    NODE_INT,
    NODE_STRING,
    NODE_ARRAY_LIT,
    NODE_VAR,          // variable reference
    NODE_VAR_DECL,     // var name = expr;
    NODE_ASSIGN,       // name = expr;
    NODE_INDEX_ASSIGN, // arr[idx] = expr;
    NODE_BINARY,
    NODE_UNARY,
    NODE_IF,
    NODE_WHILE,
    NODE_RETURN,
    NODE_FUNC,
    NODE_CALL,
    NODE_INDEX,        // arr[idx]
    NODE_PRINT,
    NODE_BLOCK,
    NODE_EXPR_STMT,
    NODE_NIL,
    NODE_BREAK,
} NodeType;

typedef struct Node Node;

struct Node {
    NodeType type;
    int line;
    union {
        struct { int64_t value; } integer;
        struct { char *value; } string;
        struct { Node **items; int count; } array_lit;
        struct { char *name; } var;
        struct { char *name; Node *value; } var_decl;
        struct { char *name; Node *value; } assign;
        struct { Node *target; Node *index; Node *value; } index_assign;
        struct { Node *left; Node *right; int op; } binary; // op is TokenType
        struct { Node *operand; int op; } unary;
        struct { Node *cond; Node *then_branch; Node *else_branch; } if_stmt;
        struct { Node *cond; Node *body; } while_stmt;
        struct { Node *value; } return_stmt;
        struct { char *name; char **params; int param_count; Node *body; int is_native; } func;
        struct { char *name; Node **args; int arg_count; } call;
        struct { Node *array; Node *index; } index;
        struct { Node *value; int newline; } print;
        struct { Node **stmts; int count; } block;
        struct { Node *expr; } expr_stmt;
    };
};

Node *node_new(NodeType type, int line);
void node_free(Node *n);

/* ===================== Parser ===================== */
typedef struct {
    char *module_name;  // e.g. "math"
    char **exports;     // exported function names as imported, e.g. "cos"
    int export_count;
    int export_cap;
} ModuleInfo;
typedef struct {
    Lexer *lexer;
    int pos;
    Node **funcs;   // top-level functions
    int func_count;
    int func_cap;
    Node **globals; // top-level var declarations
    int global_count;
    int global_cap;
    ModuleInfo *modules;  // loaded modules (for namespace access)
    int module_count;
    int module_cap;
    char **alias_names;   // require cos from math -> alias "cos"
    char **alias_targets; // -> target "math.cos"
    int alias_count;
    int alias_cap;
} Parser;

Parser *parser_new(Lexer *l);
void parser_free(Parser *p);
void parse(Parser *p);
void parser_set_search_dirs(const char *script_dir, const char *syslib_dir);

/* ===================== Bytecode ===================== */
typedef enum {
    OP_CONST,
    OP_NIL,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_NEG,
    OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LTE, OP_GTE,
    OP_AND, OP_OR, OP_NOT,
    OP_LOAD,      // op1: local index
    OP_STORE,     // op1: local index
    OP_PRINT,
    OP_PRINTLN,
    OP_JMP,       // op1: instruction offset
    OP_JMPF,      // op1: instruction offset
    OP_CALL,      // op1: function index, op2: arg count
    OP_RET,
    OP_ARRAY,     // op1: element count
    OP_INDEX,
    OP_INDEX_SET,
    OP_POP,
    OP_HALT,
    OP_CONCAT,
    OP_LEN,
    OP_CHARAT,
    OP_SUBSTR,
    OP_TOSTRING,
    OP_TOINT,
    OP_STRCMP,
    OP_LOAD_GLOBAL,   // op1: global index
    OP_STORE_GLOBAL,  // op1: global index
    OP_READALL,       // read all stdin, push as string
    OP_MAKE_ARRAY,    // pop init, pop size, create array of size with init
    OP_ARGC,          // push command-line argument count
    OP_ARGV,          // pop index, push argv[index] as string
    OP_READFILE,      // pop path, push file contents as string
    OP_FILEEXISTS,    // pop path, push 1 if file exists else 0
    OP_WRITEFILE,     // pop content, pop path, write file, push 1 on success
    OP_SYSTEM,        // pop command, execute via system(), push exit code
    OP_READLINE,      // read one line from stdin, push as string ("" on EOF)
    OP_READFILEBYTES, // pop path, push file contents as int array (one int per byte)
    OP_WRITEFILEBYTES,// pop byte array, pop path, write raw bytes, push 1 on success
    OP_CHR,           // pop int, push 1-char string of that byte value
    OP_NATIVE,        // op1: native func index (into funcs[], is_native), op2: arg count
} OpCode;

typedef struct {
    int *code;     // instructions, each 3 ints: [op, op1, op2]
    int count;
    int cap;
} Bytecode;

typedef struct {
    char *name;
    int address;   // instruction index; -1 for native (C-implemented) functions
    int param_count;
    int local_count;
    int is_native; // 1: implemented in C via the native registry, not bytecode
} FuncInfo;

typedef struct {
    Value *constants;
    int const_count;
    int const_cap;
    FuncInfo *funcs;
    int func_count;
    int func_cap;
    char **global_names;
    int global_count;
    int global_cap;
    Bytecode bc;
} Program;

Program *program_new(void);
void program_free(Program *prog);
int program_add_const(Program *prog, Value v);
int program_add_func(Program *prog, const char *name, int address, int params);
void bc_emit(Program *prog, int op, int op1, int op2);

Program *compile_to_bytecode(Parser *p);
/* ===================== .milc bytecode serialization ===================== */
#define MILC_MAGIC "!milc"
#define MILC_MAGIC_LEN 5
int program_write_milc(Program *prog, const char *path);  // 0 on success
Program *program_read_milc(const char *path);             // NULL on error
int is_milc_file(const char *path);

/* ===================== VM ===================== */
#define VM_STACK_SIZE 16384
#define VM_CALL_STACK 8192

typedef struct {
    int func_index;
    int ret_addr;    // return address (instruction index)
    Value *old_locals; // previous frame's locals pointer
} CallFrame;

typedef struct {
    Program *prog;
    Value stack[VM_STACK_SIZE];
    int sp;
    Value *locals;    // current frame locals, allocated per call
    Value *globals;   // global variables
    CallFrame calls[VM_CALL_STACK];
    int call_depth;
    int ip;
    int argc;         // command-line argument count
    char **argv;      // command-line arguments (argv[0] = program name)
} VM;

VM *vm_new(Program *prog);
void vm_free(VM *vm);
void vm_set_args(VM *vm, int argc, char **argv);
Value vm_run(VM *vm);

/* ===================== Native (C) method registry ===================== */
/* A native method is a C function callable from minilang. Arguments are
   popped onto `args` (args[0] is the first argument), `argc` is the count.
   Return value is pushed back onto the minilang stack. */
typedef Value (*NativeFn)(VM *vm, Value *args, int argc);

/* Register a C native method under `name`. Overwrites any previous entry. */
void ml_native_register(const char *name, NativeFn fn);
/* Look up a native method by name; returns NULL if not registered. */
NativeFn ml_native_lookup(const char *name);
/* Ensure the built-in native method table (syslib natives) is registered. */
void ml_native_register_builtins(void);

/* ===================== LLVM IR Gen ===================== */
char *generate_llvm_ir(Parser *p);

/* ===================== Utility ===================== */
char *read_file(const char *path);
void die(const char *fmt, ...);

#endif /* MINILANG_H */
