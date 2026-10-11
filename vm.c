#include "minilang.h"

VM *vm_new(Program *prog) {
    VM *vm = calloc(1, sizeof(VM));
    vm->prog = prog;
    vm->sp = 0;
    vm->call_depth = 0;
    vm->ip = 0;
    vm->locals = NULL;
    vm->globals = calloc(prog->global_count > 0 ? prog->global_count : 1, sizeof(Value));
    return vm;
}

void vm_free(VM *vm) {
    if (vm->locals) free(vm->locals);
    if (vm->globals) free(vm->globals);
    free(vm);
}

void vm_set_args(VM *vm, int argc, char **argv) {
    vm->argc = argc;
    vm->argv = argv;
}

static inline void push(VM *vm, Value v) {
    if (vm->sp >= VM_STACK_SIZE) {
        fprintf(stderr, "VM error: stack overflow (sp=%d, call_depth=%d, ip=%d)\n", vm->sp, vm->call_depth, vm->ip);
        exit(1);
    }
    vm->stack[vm->sp++] = v;
}

static inline Value pop(VM *vm) {
    if (vm->sp <= 0) {
        fprintf(stderr, "VM error: stack underflow\n");
        exit(1);
    }
    return vm->stack[--vm->sp];
}

static inline Value peek_top(VM *vm) __attribute__((unused));
static inline Value peek_top(VM *vm) {
    if (vm->sp <= 0) {
        fprintf(stderr, "VM error: stack underflow in peek\n");
        exit(1);
    }
    return vm->stack[vm->sp - 1];
}

Value vm_run(VM *vm) {
    Program *prog = vm->prog;
    Value result = make_nil();

    while (1) {
        if (vm->ip >= prog->bc.count / 3) {
            fprintf(stderr, "VM error: instruction pointer out of bounds (%d)\n", vm->ip);
            exit(1);
        }
        int base = vm->ip * 3;
        int op = prog->bc.code[base];
        int op1 = prog->bc.code[base + 1];
        int op2 = prog->bc.code[base + 2];
        vm->ip++;

        switch (op) {
            case OP_CONST: {
                Value v = prog->constants[op1];
                // Copy string/array (constants are shared, but we need ownership tracking)
                // For simplicity, constants are never freed; values on stack reference them.
                // But operations may create new values that need freeing.
                // We'll do a shallow copy; strings from constants are shared.
                push(vm, v);
                break;
            }
            case OP_NIL:
                push(vm, make_nil());
                break;
            case OP_ADD: {
                Value b = pop(vm), a = pop(vm);
                push(vm, value_add(a, b));
                break;
            }
            case OP_SUB: {
                Value b = pop(vm), a = pop(vm);
                push(vm, value_sub(a, b));
                break;
            }
            case OP_MUL: {
                Value b = pop(vm), a = pop(vm);
                push(vm, value_mul(a, b));
                break;
            }
            case OP_DIV: {
                Value b = pop(vm), a = pop(vm);
                push(vm, value_div(a, b));
                break;
            }
            case OP_MOD: {
                Value b = pop(vm), a = pop(vm);
                push(vm, value_mod(a, b));
                break;
            }
            case OP_NEG: {
                Value a = pop(vm);
                push(vm, value_neg(a));
                break;
            }
            case OP_EQ: {
                Value b = pop(vm), a = pop(vm);
                push(vm, value_eq(a, b));
                break;
            }
            case OP_NEQ: {
                Value b = pop(vm), a = pop(vm);
                push(vm, value_neq(a, b));
                break;
            }
            case OP_LT: {
                Value b = pop(vm), a = pop(vm);
                push(vm, value_lt(a, b));
                break;
            }
            case OP_GT: {
                Value b = pop(vm), a = pop(vm);
                push(vm, value_gt(a, b));
                break;
            }
            case OP_LTE: {
                Value b = pop(vm), a = pop(vm);
                push(vm, value_lte(a, b));
                break;
            }
            case OP_GTE: {
                Value b = pop(vm), a = pop(vm);
                push(vm, value_gte(a, b));
                break;
            }
            case OP_AND: {
                Value b = pop(vm), a = pop(vm);
                push(vm, value_and(a, b));
                break;
            }
            case OP_OR: {
                Value b = pop(vm), a = pop(vm);
                push(vm, value_or(a, b));
                break;
            }
            case OP_NOT: {
                Value a = pop(vm);
                push(vm, value_not(a));
                break;
            }
            case OP_LOAD:
                push(vm, vm->locals[op1]);
                break;
            case OP_STORE:
                vm->locals[op1] = pop(vm);
                break;
            case OP_LOAD_GLOBAL:
                push(vm, vm->globals[op1]);
                break;
            case OP_STORE_GLOBAL:
                vm->globals[op1] = pop(vm);
                break;
            case OP_READALL: {
                // Read all of stdin
                size_t cap = 4096, len = 0;
                char *buf = malloc(cap);
                int c;
                while ((c = fgetc(stdin)) != EOF) {
                    if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                    buf[len++] = (char)c;
                }
                buf[len] = '\0';
                push(vm, make_string(buf));
                free(buf);
                break;
            }
            case OP_MAKE_ARRAY: {
                Value init = pop(vm);
                Value sizev = pop(vm);
                int size = (int)sizev.as.integer;
                if (size < 0) size = 0;
                Value *items = malloc(sizeof(Value) * (size > 0 ? size : 1));
                for (int i = 0; i < size; i++) items[i] = init;
                push(vm, make_array(items, size));
                free(items);
                break;
            }
            case OP_ARGC: {
                push(vm, make_int(vm->argc));
                break;
            }
            case OP_ARGV: {
                Value idxv = pop(vm);
                int idx = (int)idxv.as.integer;
                if (idx < 0 || idx >= vm->argc) {
                    push(vm, make_string(""));
                } else {
                    push(vm, make_string(vm->argv[idx]));
                }
                break;
            }
            case OP_READFILE: {
                Value pathv = pop(vm);
                const char *path = pathv.as.string;
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
                push(vm, make_string(buf));
                free(buf);
                break;
            }
            case OP_FILEEXISTS: {
                Value pathv = pop(vm);
                const char *path = pathv.as.string;
                FILE *f = fopen(path, "rb");
                if (f) { fclose(f); push(vm, make_int(1)); }
                else { push(vm, make_int(0)); }
                break;
            }
            case OP_WRITEFILE: {
                Value contentv = pop(vm);
                Value pathv = pop(vm);
                const char *path = pathv.as.string;
                const char *content = contentv.as.string;
                FILE *f = fopen(path, "w");
                if (f) {
                    fwrite(content, 1, strlen(content), f);
                    fclose(f);
                    push(vm, make_int(1));
                } else {
                    push(vm, make_int(0));
                }
                break;
            }
            case OP_SYSTEM: {
                Value cmdv = pop(vm);
                const char *cmd = cmdv.as.string;
                int rc = system(cmd);
                push(vm, make_int(rc));
                break;
            }
            case OP_READLINE: {
                char buf[8192];
                if (fgets(buf, sizeof(buf), stdin) == NULL) {
                    push(vm, make_string(""));
                } else {
                    size_t n = strlen(buf);
                    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
                    push(vm, make_string(buf));
                }
                break;
            }
            case OP_READFILEBYTES: {
                // Read a file as raw bytes: push an int array, one element per byte (0..255)
                Value pathv = pop(vm);
                const char *path = pathv.as.string;
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
                while (n < size && (c = fgetc(f)) != EOF) items[n++] = make_int((unsigned char)c);
                fclose(f);
                push(vm, make_array(items, (int)n));
                free(items);
                break;
            }
            case OP_CHR: {
                Value nv = pop(vm);
                char buf[2];
                buf[0] = (char)(unsigned char)(nv.as.integer & 0xFF);
                buf[1] = '\0';
                push(vm, make_string(buf));
                break;
            }
            case OP_NATIVE: {
                // Pop arguments (args[0] is the first argument), then call the
                // registered C function under this func's name.
                Value args[256];
                for (int i = op2 - 1; i >= 0; i--) args[i] = pop(vm);
                const char *name = prog->funcs[op1].name;
                NativeFn fn = ml_native_lookup(name);
                if (!fn) {
                    fprintf(stderr, "VM error: native function '%s' is not registered\n", name);
                    exit(1);
                }
                Value ret = fn(vm, args, op2);
                push(vm, ret);
                break;
            }
            case OP_WRITEFILEBYTES: {
                // Write an int array as raw bytes: pop array, pop path
                Value arrv = pop(vm);
                Value pathv = pop(vm);
                const char *path = pathv.as.string;
                FILE *f = fopen(path, "wb");
                if (!f) { push(vm, make_int(0)); break; }
                int ok = 1;
                if (arrv.type == VAL_ARRAY) {
                    for (int i = 0; i < arrv.as.array.count; i++) {
                        unsigned char b = 0;
                        Value it = arrv.as.array.items[i];
                        if (it.type == VAL_INT) b = (unsigned char)(it.as.integer & 0xFF);
                        else if (it.type == VAL_STRING && it.as.string && it.as.string[0])
                            b = (unsigned char)it.as.string[0];
                        if (fwrite(&b, 1, 1, f) != 1) { ok = 0; break; }
                    }
                }
                fclose(f);
                push(vm, make_int(ok));
                break;
            }
            case OP_PRINT: {
                Value v = pop(vm);
                value_print(v, 0);
                break;
            }
            case OP_PRINTLN: {
                Value v = pop(vm);
                value_print(v, 1);
                break;
            }
            case OP_JMP:
                vm->ip = op1;
                break;
            case OP_JMPF: {
                Value v = pop(vm);
                if (!value_is_truthy(v)) vm->ip = op1;
                break;
            }
            case OP_CALL: {
                FuncInfo *fi = &prog->funcs[op1];
                if (vm->call_depth >= VM_CALL_STACK) {
                    fprintf(stderr, "VM error: call stack overflow\n");
                    exit(1);
                }
                // Pop arguments
                Value args[256];
                for (int i = op2 - 1; i >= 0; i--) args[i] = pop(vm);
                // Save frame
                CallFrame *frame = &vm->calls[vm->call_depth++];
                frame->func_index = op1;
                frame->ret_addr = vm->ip;
                frame->old_locals = vm->locals;
                // Allocate locals
                vm->locals = calloc(fi->local_count, sizeof(Value));
                for (int i = 0; i < op2 && i < fi->local_count; i++) {
                    vm->locals[i] = args[i];
                }
                vm->ip = fi->address;
                break;
            }
            case OP_RET: {
                Value ret_val = pop(vm);
                CallFrame *frame = &vm->calls[--vm->call_depth];
                Value *old_locals = frame->old_locals;
                free(vm->locals);
                vm->locals = old_locals;
                vm->ip = frame->ret_addr;
                push(vm, ret_val);
                break;
            }
            case OP_ARRAY: {
                Value *items = malloc(sizeof(Value) * op1);
                for (int i = op1 - 1; i >= 0; i--) items[i] = pop(vm);
                push(vm, make_array(items, op1));
                free(items);
                break;
            }
            case OP_INDEX: {
                Value idx = pop(vm);
                Value arr = pop(vm);
                if (arr.type == VAL_ARRAY) {
                    int i = (int)idx.as.integer;
                    if (i < 0 || i >= arr.as.array.count) {
                        fprintf(stderr, "VM error: array index %d out of bounds (size %d)\n", i, arr.as.array.count);
                        exit(1);
                    }
                    push(vm, arr.as.array.items[i]);
                } else if (arr.type == VAL_STRING) {
                    push(vm, value_charat(arr, idx));
                } else {
                    fprintf(stderr, "VM error: cannot index non-array/string\n");
                    exit(1);
                }
                break;
            }
            case OP_INDEX_SET: {
                Value val = pop(vm);
                Value idx = pop(vm);
                Value arr = pop(vm);
                if (arr.type == VAL_ARRAY) {
                    int i = (int)idx.as.integer;
                    if (i < 0 || i >= arr.as.array.count) {
                        fprintf(stderr, "VM error: array index %d out of bounds (size %d)\n", i, arr.as.array.count);
                        exit(1);
                    }
                    arr.as.array.items[i] = val;
                } else {
                    fprintf(stderr, "VM error: cannot index-assign non-array\n");
                    exit(1);
                }
                break;
            }
            case OP_POP:
                pop(vm);
                break;
            case OP_HALT:
                result = pop(vm);
                goto done;
            case OP_CONCAT: {
                Value b = pop(vm), a = pop(vm);
                push(vm, value_add(a, b)); // + handles string concat
                break;
            }
            case OP_LEN: {
                Value v = pop(vm);
                push(vm, value_len(v));
                break;
            }
            case OP_CHARAT: {
                Value idx = pop(vm), s = pop(vm);
                push(vm, value_charat(s, idx));
                break;
            }
            case OP_SUBSTR: {
                Value len = pop(vm), start = pop(vm), s = pop(vm);
                push(vm, value_substr(s, start, len));
                break;
            }
            case OP_TOSTRING: {
                Value v = pop(vm);
                push(vm, value_tostring(v));
                break;
            }
            case OP_TOINT: {
                Value v = pop(vm);
                push(vm, value_toint(v));
                break;
            }
            case OP_STRCMP: {
                Value b = pop(vm), a = pop(vm);
                push(vm, value_strcmp(a, b));
                break;
            }
            default:
                fprintf(stderr, "VM error: unknown opcode %d at ip %d\n", op, vm->ip - 1);
                exit(1);
        }
    }
done:
    return result;
}
