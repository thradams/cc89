/*
 * Assembler, the part shared by every instruction set: the text written
 * by a back-end -> sections, symbols and fixups (struct object).
 *
 * It understands only the directives the back-ends use. Keeping the
 * code generator on text makes it easy to read (cc89 -S shows it).
 *
 *   label:              defines a symbol at the current position
 *   .text .data ...     selects the section
 *   .byte .quad .zero   data
 *   .loc .file .func    debug and unwind information
 *   anything else       an instruction: asm_x64.c or asm_arm64.c
 */
#include "asm.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------ symbols */

static unsigned hash(const char* s)
{
    unsigned h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

struct symbol* find_symbol(struct object* obj, const char* name, int create)
{
    unsigned h = hash(name) % SYMBOL_BUCKETS;
    for (struct symbol* s = obj->buckets[h]; s; s = s->next)
        if (strcmp(s->name, name) == 0)
            return s;
    if (!create)
        return NULL;
    struct symbol* s = cc_alloc(sizeof *s);
    s->name = cc_strndup(name, (int)strlen(name));
    s->next = obj->buckets[h];
    obj->buckets[h] = s;
    return s;
}

/* -------------------------------------------------------------- bytes */

void asm_error(struct assembler* a, const char* msg, const char* line)
{
    cc_error("<asm>", a->line, 1, "%s: %s", msg, line);
}

void out8(struct assembler* a, int b)
{
    struct section* s = &a->obj->sections[a->sec];
    if (s->size == s->capacity)
    {
        s->capacity = s->capacity ? s->capacity * 2 : 4096;
        s->data = realloc(s->data, s->capacity);
        if (s->data == NULL)
        {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
    }
    s->data[s->size++] = (unsigned char)b;
}

void out_n(struct assembler* a, unsigned long long v, int n)
{
    for (int i = 0; i < n; i++)
        out8(a, (int)(v >> (8 * i)) & 0xFF);
}

int here(struct assembler* a)
{
    return a->obj->sections[a->sec].size;
}

void add_fixup(struct assembler* a, enum fixup_kind kind, int offset, int end, const char* name, long long addend)
{
    struct fixup* f = cc_alloc(sizeof *f);
    f->kind = kind;
    f->section = a->sec;
    f->offset = offset;
    f->end = end;
    f->name = name;
    f->addend = addend;
    f->next = a->obj->fixups;
    a->obj->fixups = f;
}

long long parse_number(const char* s)
{
    if (*s == '-') return strtoll(s, NULL, 0);
    return (long long)strtoull(s, NULL, 0);
}

int is_sym_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '.' || c == '$';
}

/* "name+12" or "name-4" or "name" */
const char* parse_sym_addend(const char* s, long long* addend)
{
    const char* p = s;
    while (is_sym_char(*p)) p++;
    *addend = *p ? parse_number(p) : 0;
    return cc_strndup(s, (int)(p - s));
}

/* ---------------------------------------------------------- directives */

static void directive(struct assembler* a, char* s, const char* line)
{
    char name[32];
    int n = 0;
    while (s[n] && s[n] != ' ' && n < 31) { name[n] = s[n]; n++; }
    name[n] = 0;
    char* args = s + n;
    while (*args == ' ') args++;

    if (!strcmp(name, ".text")) { a->sec = SEC_TEXT; return; }
    if (!strcmp(name, ".section") && !strncmp(args, ".debug_info", 11)) { a->sec = SEC_DEBUG_INFO; return; }
    if (!strcmp(name, ".section") && !strncmp(args, ".debug_abbrev", 13)) { a->sec = SEC_DEBUG_ABBREV; return; }
    if (!strcmp(name, ".section") && !strncmp(args, ".debug$S", 8)) { a->sec = SEC_DEBUG_S; return; }
    if (!strcmp(name, ".section") && !strncmp(args, ".debug$T", 8)) { a->sec = SEC_DEBUG_T; return; }

    /* CodeView addresses: offset of a symbol in its section, and the section */
    if (!strcmp(name, ".secrel32") || !strcmp(name, ".secidx"))
    {
        int is_idx = name[4] == 'i';
        long long addend;
        const char* sym = parse_sym_addend(args, &addend);
        add_fixup(a, is_idx ? FIX_SECIDX : FIX_SECREL32, here(a), 0, sym, addend);
        out_n(a, 0, is_idx ? 2 : 4);
        return;
    }
    if (!strcmp(name, ".data") || !strcmp(name, ".bss") || !strcmp(name, ".section")) { a->sec = SEC_DATA; return; }

    /* debug information: .file N "name", .loc N line, .func name, .endfunc */
    if (!strcmp(name, ".file"))
    {
        int n = atoi(args);
        char* q1 = strchr(args, '"');
        char* q2 = q1 ? strrchr(q1 + 1, '"') : NULL;
        if (n <= 0 || n >= 256 || q2 == NULL)
            asm_error(a, "invalid .file", line);
        a->obj->files[n] = cc_strndup(q1 + 1, (int)(q2 - q1 - 1));
        return;
    }
    if (!strcmp(name, ".loc"))
    {
        struct line_row* r = cc_alloc(sizeof *r);
        char* end;
        r->file = (int)strtol(args, &end, 10);
        r->line = (int)strtol(end, NULL, 10);
        r->offset = here(a);
        if (a->obj->lines_tail) a->obj->lines_tail->next = r;
        else a->obj->lines = r;
        a->obj->lines_tail = r;
        return;
    }
    if (!strcmp(name, ".func"))
    {
        struct func_range* f = cc_alloc(sizeof *f);
        f->name = cc_strndup(args, (int)strlen(args));
        f->start = here(a);
        if (a->obj->funcs_tail) a->obj->funcs_tail->next = f;
        else a->obj->funcs = f;
        a->obj->funcs_tail = f;
        return;
    }
    if (!strcmp(name, ".endfunc"))
    {
        if (a->obj->funcs_tail)
            a->obj->funcs_tail->end = here(a);
        return;
    }
    if (!strcmp(name, ".globl")) return;   /* one image: every name is visible */
    if (!strcmp(name, ".selectany")) { a->selectany_next = 1; return; }
    if (!strcmp(name, ".extern_data"))
    {
        find_symbol(a->obj, args, 1)->is_data = 1;
        return;
    }

    if (!strcmp(name, ".balign"))
    {
        int align = atoi(args);
        while (here(a) % align) out8(a, a->sec == SEC_TEXT && !a->obj->arm64 ? 0xCC : 0);
        return;
    }
    if (!strcmp(name, ".zero"))
    {
        for (int i = atoi(args); i > 0; i--) out8(a, 0);
        return;
    }
    if (!strcmp(name, ".byte"))
    {
        for (char* p = strtok(args, ","); p; p = strtok(NULL, ","))
            out8(a, (int)parse_number(p) & 0xFF);
        return;
    }
    if (!strcmp(name, ".quad"))
    {
        if (isdigit((unsigned char)*args) || *args == '-')
        {
            out_n(a, (unsigned long long)parse_number(args), 8);
            return;
        }
        long long addend;
        const char* sym = parse_sym_addend(args, &addend);
        add_fixup(a, FIX_ABS64, here(a), 0, sym, addend);
        out_n(a, 0, 8);
        return;
    }
    asm_error(a, "unknown directive", line);
}

/* ------------------------------------------------------------- driver */

void assemble(const char* text, struct object* obj)
{
    struct assembler a = { 0 };
    a.obj = obj;
    a.sec = SEC_TEXT;

    const char* p = text;
    while (*p)
    {
        const char* eol = strchr(p, '\n');
        int len = eol ? (int)(eol - p) : (int)strlen(p);
        char line[1024];
        if (len >= (int)sizeof line) len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = 0;
        p += len + (eol ? 1 : 0);
        a.line++;

        /* comment: # on x64, // on arm64 (where # starts a number); not inside a string */
        int in_string = 0;
        for (char* c = line; *c; c++)
        {
            if (*c == '"') in_string = !in_string;
            if (in_string) continue;
            if (obj->arm64 ? (c[0] == '/' && c[1] == '/') : *c == '#') { *c = 0; break; }
        }

        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        int n = (int)strlen(s);
        while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\r')) s[--n] = 0;
        if (n == 0)
            continue;

        /* label */
        if (s[n - 1] == ':')
        {
            s[n - 1] = 0;
            struct symbol* sym = find_symbol(obj, s, 1);
            if (sym->defined)
            {
                /* selectany (COMDAT): keep the first copy, the bytes of
                   this one stay in the section but nobody refers to them */
                if (sym->selectany && a.selectany_next)
                {
                    a.selectany_next = 0;
                    continue;
                }
                asm_error(&a, "symbol defined twice", s);
            }
            sym->selectany = a.selectany_next;
            a.selectany_next = 0;
            sym->defined = 1;
            sym->section = a.sec;
            sym->offset = here(&a);
            continue;
        }

        if (s[0] == '.')
        {
            directive(&a, s, line);
            continue;
        }

        if (obj->arm64) arm64_line(&a, s, line);
        else x64_line(&a, s, line);
    }
}
