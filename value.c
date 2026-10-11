#include "minilang.h"

Value make_nil(void) {
    Value v;
    v.type = VAL_NIL;
    v.as.integer = 0;
    return v;
}

Value make_int(int64_t v) {
    Value r;
    r.type = VAL_INT;
    r.as.integer = v;
    return r;
}

Value make_string(const char *s) {
    Value r;
    r.type = VAL_STRING;
    r.as.string = strdup(s ? s : "");
    return r;
}

Value make_string_n(const char *s, int n) {
    Value r;
    r.type = VAL_STRING;
    r.as.string = malloc(n + 1);
    memcpy(r.as.string, s, n);
    r.as.string[n] = '\0';
    return r;
}

Value make_array(Value *items, int count) {
    Value r;
    r.type = VAL_ARRAY;
    r.as.array.count = count;
    r.as.array.cap = count > 0 ? count : 4;
    r.as.array.items = malloc(sizeof(Value) * r.as.array.cap);
    for (int i = 0; i < count; i++) {
        r.as.array.items[i] = items[i];
    }
    return r;
}

void value_free(Value v) {
    if (v.type == VAL_STRING) free(v.as.string);
    else if (v.type == VAL_ARRAY) {
        for (int i = 0; i < v.as.array.count; i++)
            value_free(v.as.array.items[i]);
        free(v.as.array.items);
    }
}

void value_print(Value v, int newline) {
    switch (v.type) {
        case VAL_NIL: printf("nil"); break;
        case VAL_INT: printf("%lld", (long long)v.as.integer); break;
        case VAL_STRING: printf("%s", v.as.string); break;
        case VAL_ARRAY: {
            printf("[");
            for (int i = 0; i < v.as.array.count; i++) {
                if (i > 0) printf(", ");
                value_print(v.as.array.items[i], 0);
            }
            printf("]");
            break;
        }
    }
    if (newline) printf("\n");
    fflush(stdout);
}

int value_is_truthy(Value v) {
    switch (v.type) {
        case VAL_NIL: return 0;
        case VAL_INT: return v.as.integer != 0;
        case VAL_STRING: return strlen(v.as.string) > 0;
        case VAL_ARRAY: return 1;
    }
    return 0;
}

static void type_error(const char *op, Value a, Value b) {
    fprintf(stderr, "Type error in %s: ", op);
    value_print(a, 0);
    fprintf(stderr, " and ");
    value_print(b, 0);
    fprintf(stderr, "\n");
    exit(1);
}

Value value_add(Value a, Value b) {
    if (a.type == VAL_INT && b.type == VAL_INT)
        return make_int(a.as.integer + b.as.integer);
    if (a.type == VAL_STRING || b.type == VAL_STRING) {
        char sa[64], sb[64];
        const char *pa, *pb;
        if (a.type == VAL_STRING) pa = a.as.string;
        else { snprintf(sa, sizeof(sa), "%lld", (long long)a.as.integer); pa = sa; }
        if (b.type == VAL_STRING) pb = b.as.string;
        else { snprintf(sb, sizeof(sb), "%lld", (long long)b.as.integer); pb = sb; }
        int la = strlen(pa), lb = strlen(pb);
        char *buf = malloc(la + lb + 1);
        memcpy(buf, pa, la);
        memcpy(buf + la, pb, lb);
        buf[la + lb] = '\0';
        Value r = make_string(buf);
        free(buf);
        return r;
    }
    type_error("+", a, b);
    return make_nil();
}

Value value_sub(Value a, Value b) {
    if (a.type == VAL_INT && b.type == VAL_INT)
        return make_int(a.as.integer - b.as.integer);
    type_error("-", a, b);
    return make_nil();
}

Value value_mul(Value a, Value b) {
    if (a.type == VAL_INT && b.type == VAL_INT)
        return make_int(a.as.integer * b.as.integer);
    type_error("*", a, b);
    return make_nil();
}

Value value_div(Value a, Value b) {
    if (a.type == VAL_INT && b.type == VAL_INT) {
        if (b.as.integer == 0) { fprintf(stderr, "Division by zero\n"); exit(1); }
        return make_int(a.as.integer / b.as.integer);
    }
    type_error("/", a, b);
    return make_nil();
}

Value value_mod(Value a, Value b) {
    if (a.type == VAL_INT && b.type == VAL_INT) {
        if (b.as.integer == 0) { fprintf(stderr, "Modulo by zero\n"); exit(1); }
        return make_int(a.as.integer % b.as.integer);
    }
    type_error("%%", a, b);
    return make_nil();
}

Value value_neg(Value a) {
    if (a.type == VAL_INT) return make_int(-a.as.integer);
    fprintf(stderr, "Type error in unary -\n");
    exit(1);
}

static int cmp_values(Value a, Value b) {
    if (a.type == VAL_INT && b.type == VAL_INT) {
        if (a.as.integer < b.as.integer) return -1;
        if (a.as.integer > b.as.integer) return 1;
        return 0;
    }
    if (a.type == VAL_STRING && b.type == VAL_STRING)
        return strcmp(a.as.string, b.as.string);
    if (a.type == VAL_NIL && b.type == VAL_NIL) return 0;
    return -2; // incomparable
}

Value value_eq(Value a, Value b) { return make_int(cmp_values(a, b) == 0); }
Value value_neq(Value a, Value b) { return make_int(cmp_values(a, b) != 0); }
Value value_lt(Value a, Value b) { return make_int(cmp_values(a, b) < 0); }
Value value_gt(Value a, Value b) { return make_int(cmp_values(a, b) > 0); }
Value value_lte(Value a, Value b) { return make_int(cmp_values(a, b) <= 0); }
Value value_gte(Value a, Value b) { return make_int(cmp_values(a, b) >= 0); }

Value value_and(Value a, Value b) {
    return make_int(value_is_truthy(a) && value_is_truthy(b));
}
Value value_or(Value a, Value b) {
    return make_int(value_is_truthy(a) || value_is_truthy(b));
}
Value value_not(Value a) { return make_int(!value_is_truthy(a)); }

Value value_len(Value v) {
    if (v.type == VAL_STRING) return make_int(strlen(v.as.string));
    if (v.type == VAL_ARRAY) return make_int(v.as.array.count);
    fprintf(stderr, "len() expects string or array\n");
    exit(1);
}

Value value_charat(Value s, Value idx) {
    if (s.type != VAL_STRING || idx.type != VAL_INT) {
        fprintf(stderr, "charAt expects string and int\n");
        exit(1);
    }
    int i = (int)idx.as.integer;
    int len = strlen(s.as.string);
    if (i < 0 || i >= len) return make_int(0);
    return make_int((unsigned char)s.as.string[i]);
}

Value value_substr(Value s, Value start, Value len) {
    if (s.type != VAL_STRING || start.type != VAL_INT || len.type != VAL_INT) {
        fprintf(stderr, "substr expects string, int, int\n");
        exit(1);
    }
    int st = (int)start.as.integer;
    int ln = (int)len.as.integer;
    int slen = strlen(s.as.string);
    if (st < 0) st = 0;
    if (st > slen) st = slen;
    if (st + ln > slen) ln = slen - st;
    if (ln < 0) ln = 0;
    return make_string_n(s.as.string + st, ln);
}

Value value_tostring(Value v) {
    if (v.type == VAL_STRING) return make_string(v.as.string);
    if (v.type == VAL_INT) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)v.as.integer);
        return make_string(buf);
    }
    if (v.type == VAL_NIL) return make_string("nil");
    if (v.type == VAL_ARRAY) return make_string("<array>");
    return make_string("");
}

Value value_toint(Value v) {
    if (v.type == VAL_INT) return make_int(v.as.integer);
    if (v.type == VAL_STRING) return make_int(atoll(v.as.string));
    fprintf(stderr, "toInt expects int or string\n");
    exit(1);
}

Value value_strcmp(Value a, Value b) {
    if (a.type == VAL_STRING && b.type == VAL_STRING)
        return make_int(strcmp(a.as.string, b.as.string));
    fprintf(stderr, "strcmp expects two strings\n");
    exit(1);
}
