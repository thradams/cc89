/*
 * Lexer: text -> tokens.
 *
 * Handles comments, line markers and #pragma.
 * Cake output has no backslash-newline, no adjacent string literals and
 * no character constants, so they are not handled.
 */
#include "cc89.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct lexer
{
    const struct target* target;
    char* p;
    const char* file;
    int line;
    const char* line_start;
    int at_line_start;
};

static const char* keywords[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "extern", "float", "for", "goto", "if", "int",
    "long", "register", "return", "short", "signed", "sizeof", "static", "struct",
    "switch", "union", "unsigned", "void", "volatile", "while", 0
};

/* longest first: maximal munch */
static const char* puncts[] = {
    "<<=", ">>=", "...",
    "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
    "*=", "/=", "%=", "+=", "-=", "&=", "^=", "|=",
    "[", "]", "(", ")", ".", "&", "*", "+", "-", "~", "!", "/", "%",
    "<", ">", "^", "|", "?", ":", "=", ",", "{", "}", ";", 0
};

static void push(struct token_list* list, struct token* tok)
{
    if (list->size == list->capacity)
    {
        list->capacity = list->capacity ? list->capacity * 2 : 1024;
        list->data = realloc(list->data, sizeof(struct token) * list->capacity);
        if (list->data == NULL)
        {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
    }
    list->data[list->size++] = *tok;
}

static char* read_file(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (f == NULL)
    {
        fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = cc_alloc(n + 1);
    fread(buf, 1, n, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

static int col(struct lexer* lx)
{
    return (int)(lx->p - lx->line_start) + 1;
}

static int read_escape(struct lexer* lx)
{
    /* lx->p is after the backslash */
    char c = *lx->p++;
    switch (c)
    {
        case 'a': return 7;
        case 'b': return 8;
        case 'f': return 12;
        case 'n': return 10;
        case 'r': return 13;
        case 't': return 9;
        case 'v': return 11;
        case 'x':
        {
            int v = 0;
            while (isxdigit((unsigned char)*lx->p))
            {
                char d = *lx->p++;
                v = v * 16 + (isdigit((unsigned char)d) ? d - '0' : (tolower(d) - 'a' + 10));
            }
            return v;
        }
        default:
            if (c >= '0' && c <= '7')
            {
                int v = c - '0';
                for (int i = 0; i < 2 && *lx->p >= '0' && *lx->p <= '7'; i++)
                    v = v * 8 + (*lx->p++ - '0');
                return v;
            }
            return (unsigned char)c;   /* \' \" \? \\ */
    }
}

/* # 12 "file.c" flags   or   #pragma ...  */
static void read_directive(struct lexer* lx, struct token_list* out)
{
    int c0 = col(lx);
    lx->p++; /* # */
    while (*lx->p == ' ' || *lx->p == '\t') lx->p++;

    /* #line 12 "file.c" is the same as # 12 "file.c" */
    if (strncmp(lx->p, "line", 4) == 0 && (lx->p[4] == ' ' || lx->p[4] == '\t'))
    {
        lx->p += 4;
        while (*lx->p == ' ' || *lx->p == '\t') lx->p++;
    }

    if (isdigit((unsigned char)*lx->p))
    {
        int n = (int)strtol(lx->p, &lx->p, 10);
        while (*lx->p == ' ' || *lx->p == '\t') lx->p++;
        if (*lx->p == '"')
        {
            const char* start = ++lx->p;
            while (*lx->p && *lx->p != '"' && *lx->p != '\n') lx->p++;
            lx->file = cc_strndup(start, (int)(lx->p - start));
        }
        while (*lx->p && *lx->p != '\n') lx->p++;
        /* the next line has number n */
        lx->line = n - 1;
        return;
    }

    if (strncmp(lx->p, "pragma", 6) == 0)
    {
        const char* start = lx->p + 6;
        while (*lx->p && *lx->p != '\n') lx->p++;
        struct token t = { 0 };
        t.kind = TK_PRAGMA;
        t.text = cc_strndup(start, (int)(lx->p - start));
        t.file = lx->file;
        t.line = lx->line;
        t.col = c0;
        push(out, &t);
        return;
    }

    cc_error(lx->file, lx->line, c0, "unexpected preprocessor directive");
}

static void read_number(struct lexer* lx, struct token* t)
{
    const char* start = lx->p;
    int is_float = 0;

    if (lx->p[0] == '0' && (lx->p[1] == 'x' || lx->p[1] == 'X'))
    {
        lx->p += 2;
        while (isxdigit((unsigned char)*lx->p)) lx->p++;
    }
    else
    {
        while (isdigit((unsigned char)*lx->p)) lx->p++;
        if (*lx->p == '.') { is_float = 1; lx->p++; while (isdigit((unsigned char)*lx->p)) lx->p++; }
        if (*lx->p == 'e' || *lx->p == 'E')
        {
            is_float = 1;
            lx->p++;
            if (*lx->p == '+' || *lx->p == '-') lx->p++;
            while (isdigit((unsigned char)*lx->p)) lx->p++;
        }
    }

    if (is_float)
    {
        t->kind = TK_FLOAT;
        t->fval = strtod(start, NULL);
        if (*lx->p == 'f' || *lx->p == 'F') { t->is_float = 1; lx->p++; }
        else if (*lx->p == 'l' || *lx->p == 'L') lx->p++;
        return;
    }

    t->kind = TK_INT;
    int is_decimal = start[0] != '0';
    t->ival = strtoull(start, NULL, 0);

    int u = 0, l = 0;
    for (;;)
    {
        char c = *lx->p;
        if (c == 'u' || c == 'U') { u = 1; lx->p++; }
        else if (c == 'l' || c == 'L') { l++; lx->p++; }
        else break;
    }

    /* spec 1.5. With 32-bit long (Windows) long behaves as int; with
       64-bit long (LP64) it behaves as long long. So the type is int or
       long long, with the same size and signedness rules. */
    unsigned long long v = t->ival;
    int l_is_64 = l >= 2 || (l == 1 && lx->target->long_size == 8);
    if (l_is_64 || v > 0xFFFFFFFFull || (is_decimal && !u && v > 0x7FFFFFFFull))
    {
        t->is_long_long = 1;
        t->is_unsigned = u || (!is_decimal && v > 0x7FFFFFFFFFFFFFFFull);
    }
    else
    {
        t->is_unsigned = u || (!is_decimal && v > 0x7FFFFFFFull);
    }
}

static void read_string(struct lexer* lx, struct token* t)
{
    /* lx->p is at the opening quote */
    lx->p++;
    int cap = 64, len = 0;
    char* buf = cc_alloc(cap);
    while (*lx->p != '"')
    {
        if (*lx->p == 0 || *lx->p == '\n')
            cc_error(lx->file, lx->line, col(lx), "unterminated string");

        /* cake keeps backslash-newline inside strings copied from the source */
        if (lx->p[0] == '\\' && (lx->p[1] == '\n' || (lx->p[1] == '\r' && lx->p[2] == '\n')))
        {
            lx->p += lx->p[1] == '\n' ? 2 : 3;
            lx->line++;
            lx->line_start = lx->p;
            continue;
        }
        int c = *lx->p == '\\' ? (lx->p++, read_escape(lx)) : (unsigned char)*lx->p++;
        if (len + 1 >= cap)
        {
            char* nb = cc_alloc(cap * 2);
            memcpy(nb, buf, len);
            buf = nb;
            cap *= 2;
        }
        buf[len++] = (char)c;
    }
    lx->p++;
    buf[len] = 0;
    t->str = buf;
    t->str_len = len;
}

void lex_file(const char* path, struct token_list* out, const struct target* target)
{
    struct lexer lx = { 0 };
    lx.target = target;
    lx.p = read_file(path);
    lx.file = path;
    lx.line = 1;
    lx.line_start = lx.p;
    lx.at_line_start = 1;

    for (;;)
    {
        char c = *lx.p;
        if (c == 0)
            break;

        if (c == '\n')
        {
            lx.p++;
            lx.line++;
            lx.line_start = lx.p;
            lx.at_line_start = 1;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') { lx.p++; continue; }

        if (c == '/' && lx.p[1] == '*')
        {
            lx.p += 2;
            while (*lx.p && !(lx.p[0] == '*' && lx.p[1] == '/'))
            {
                if (*lx.p == '\n') { lx.p++; lx.line++; lx.line_start = lx.p; }
                else lx.p++;
            }
            if (*lx.p) lx.p += 2;
            continue;
        }

        if (c == '#' && lx.at_line_start)
        {
            read_directive(&lx, out);
            continue;
        }
        lx.at_line_start = 0;

        struct token t = { 0 };
        t.file = lx.file;
        t.line = lx.line;
        t.col = col(&lx);
        const char* start = lx.p;

        if ((isalpha((unsigned char)c) || c == '_') && !(c == 'L' && lx.p[1] == '"'))
        {
            while (isalnum((unsigned char)*lx.p) || *lx.p == '_') lx.p++;
            t.kind = TK_IDENT;
            t.text = cc_strndup(start, (int)(lx.p - start));
            for (int i = 0; keywords[i]; i++)
                if (strcmp(keywords[i], t.text) == 0) t.kind = TK_KEYWORD;
            push(out, &t);
            continue;
        }

        if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)lx.p[1])))
        {
            read_number(&lx, &t);
            t.text = cc_strndup(start, (int)(lx.p - start));
            push(out, &t);
            continue;
        }

        if (c == '"' || (c == 'L' && lx.p[1] == '"'))
        {
            if (c == 'L')
            {
                t.is_wide = 1;
                lx.p++;
            }
            t.kind = TK_STRING;
            t.text = "string literal";
            read_string(&lx, &t);
            push(out, &t);
            continue;
        }

        int found = 0;
        for (int i = 0; puncts[i]; i++)
        {
            size_t n = strlen(puncts[i]);
            if (strncmp(lx.p, puncts[i], n) == 0)
            {
                t.kind = TK_PUNCT;
                t.text = puncts[i];
                lx.p += n;
                push(out, &t);
                found = 1;
                break;
            }
        }
        if (!found)
            cc_error(lx.file, lx.line, t.col, "invalid character '%c'", c);
    }

    struct token eof = { 0 };
    eof.kind = TK_EOF;
    eof.text = "end of file";
    eof.file = lx.file;
    eof.line = lx.line;
    eof.col = 1;
    push(out, &eof);
}
