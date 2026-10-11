#include "minilang.h"

Lexer *lexer_new(const char *src) {
    Lexer *l = calloc(1, sizeof(Lexer));
    l->src = src;
    l->len = strlen(src);
    l->pos = 0;
    l->line = 1;
    l->tokens = NULL;
    l->count = 0;
    l->cap = 0;
    return l;
}

void lexer_free(Lexer *l) {
    for (int i = 0; i < l->count; i++) {
        if (l->tokens[i].text) free(l->tokens[i].text);
    }
    free(l->tokens);
    free(l);
}

static void add_token(Lexer *l, TokenType type, const char *text, int64_t intval) {
    if (l->count >= l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->tokens = realloc(l->tokens, sizeof(Token) * l->cap);
    }
    Token *t = &l->tokens[l->count++];
    t->type = type;
    t->text = text ? strdup(text) : NULL;
    t->intval = intval;
    t->line = l->line;
}

static char peek(Lexer *l) {
    return l->pos < l->len ? l->src[l->pos] : '\0';
}

static char peek_next(Lexer *l) {
    return l->pos + 1 < l->len ? l->src[l->pos + 1] : '\0';
}

static char advance(Lexer *l) {
    char c = l->src[l->pos++];
    if (c == '\n') l->line++;
    return c;
}

static void skip_whitespace_and_comments(Lexer *l) {
    while (l->pos < l->len) {
        char c = peek(l);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(l);
        } else if (c == '/' && peek_next(l) == '/') {
            while (l->pos < l->len && peek(l) != '\n') advance(l);
        } else if (c == '/' && peek_next(l) == '*') {
            advance(l); advance(l);
            while (l->pos < l->len && !(peek(l) == '*' && peek_next(l) == '/'))
                advance(l);
            if (l->pos < l->len) { advance(l); advance(l); }
        } else {
            break;
        }
    }
}

static TokenType keyword_or_ident(const char *word) {
    if (strcmp(word, "var") == 0) return TOK_VAR;
    if (strcmp(word, "func") == 0) return TOK_FUNC;
    if (strcmp(word, "if") == 0) return TOK_IF;
    if (strcmp(word, "else") == 0) return TOK_ELSE;
    if (strcmp(word, "while") == 0) return TOK_WHILE;
    if (strcmp(word, "return") == 0) return TOK_RETURN;
    if (strcmp(word, "print") == 0) return TOK_PRINT;
    if (strcmp(word, "println") == 0) return TOK_PRINTLN;
    if (strcmp(word, "break") == 0) return TOK_BREAK;
    if (strcmp(word, "require") == 0) return TOK_REQUIRE;
    if (strcmp(word, "from") == 0) return TOK_FROM;
    if (strcmp(word, "in") == 0) return TOK_IN;
    if (strcmp(word, "native") == 0) return TOK_NATIVE;
    return TOK_IDENT;
}

void lex(Lexer *l) {
    while (1) {
        skip_whitespace_and_comments(l);
        if (l->pos >= l->len) {
            add_token(l, TOK_EOF, NULL, 0);
            return;
        }
        char c = peek(l);

        // Number
        if (isdigit(c)) {
            int start = l->pos;
            while (l->pos < l->len && isdigit(peek(l))) advance(l);
            int nlen = l->pos - start;
            char *buf = malloc(nlen + 1);
            memcpy(buf, l->src + start, nlen);
            buf[nlen] = '\0';
            int64_t val = atoll(buf);
            add_token(l, TOK_INT, buf, val);
            free(buf);
            continue;
        }

        // String
        if (c == '"') {
            advance(l); // skip opening quote
            int start = l->pos;
            char *buf = malloc(l->len + 1);
            int bi = 0;
            while (l->pos < l->len && peek(l) != '"') {
                char ch = advance(l);
                if (ch == '\\' && l->pos < l->len) {
                    char esc = advance(l);
                    switch (esc) {
                        case 'n': buf[bi++] = '\n'; break;
                        case 't': buf[bi++] = '\t'; break;
                        case '\\': buf[bi++] = '\\'; break;
                        case '"': buf[bi++] = '"'; break;
                        case '0': buf[bi++] = '\0'; break;
                        default: buf[bi++] = esc; break;
                    }
                } else {
                    buf[bi++] = ch;
                }
            }
            if (l->pos < l->len) advance(l); // skip closing quote
            buf[bi] = '\0';
            add_token(l, TOK_STRING, buf, 0);
            free(buf);
            (void)start;
            continue;
        }

        // Identifier / keyword
        if (isalpha(c) || c == '_') {
            int start = l->pos;
            while (l->pos < l->len && (isalnum(peek(l)) || peek(l) == '_')) advance(l);
            int nlen = l->pos - start;
            char *buf = malloc(nlen + 1);
            memcpy(buf, l->src + start, nlen);
            buf[nlen] = '\0';
            TokenType tt = keyword_or_ident(buf);
            add_token(l, tt, buf, 0);
            free(buf);
            continue;
        }

        // Operators and punctuation
        advance(l);
        switch (c) {
            case '+': add_token(l, TOK_PLUS, NULL, 0); break;
            case '-': add_token(l, TOK_MINUS, NULL, 0); break;
            case '*': add_token(l, TOK_STAR, NULL, 0); break;
            case '/': add_token(l, TOK_SLASH, NULL, 0); break;
            case '%': add_token(l, TOK_PERCENT, NULL, 0); break;
            case '(': add_token(l, TOK_LPAREN, NULL, 0); break;
            case ')': add_token(l, TOK_RPAREN, NULL, 0); break;
            case '{': add_token(l, TOK_LBRACE, NULL, 0); break;
            case '}': add_token(l, TOK_RBRACE, NULL, 0); break;
            case '[': add_token(l, TOK_LBRACKET, NULL, 0); break;
            case ']': add_token(l, TOK_RBRACKET, NULL, 0); break;
            case ',': add_token(l, TOK_COMMA, NULL, 0); break;
            case ';': add_token(l, TOK_SEMICOLON, NULL, 0); break;
            case '.': add_token(l, TOK_DOT, NULL, 0); break;
            case '=':
                if (peek(l) == '=') { advance(l); add_token(l, TOK_EQEQ, NULL, 0); }
                else add_token(l, TOK_EQ, NULL, 0);
                break;
            case '!':
                if (peek(l) == '=') { advance(l); add_token(l, TOK_NEQ, NULL, 0); }
                else add_token(l, TOK_NOT, NULL, 0);
                break;
            case '<':
                if (peek(l) == '=') { advance(l); add_token(l, TOK_LTE, NULL, 0); }
                else add_token(l, TOK_LT, NULL, 0);
                break;
            case '>':
                if (peek(l) == '=') { advance(l); add_token(l, TOK_GTE, NULL, 0); }
                else add_token(l, TOK_GT, NULL, 0);
                break;
            case '&':
                if (peek(l) == '&') { advance(l); add_token(l, TOK_AND, NULL, 0); }
                else { fprintf(stderr, "Unexpected '&' at line %d\n", l->line); exit(1); }
                break;
            case '|':
                if (peek(l) == '|') { advance(l); add_token(l, TOK_OR, NULL, 0); }
                else { fprintf(stderr, "Unexpected '|' at line %d\n", l->line); exit(1); }
                break;
            default:
                fprintf(stderr, "Unexpected character '%c' at line %d\n", c, l->line);
                exit(1);
        }
    }
}
