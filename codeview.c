/*
 * CodeView debug information (-g on Windows): .debug$T and .debug$S.
 *
 * Every record is   u16 length (of what follows), u16 kind, data
 * and is padded to 4 bytes.
 *
 * Types (.debug$T). Each type record gets an index, starting at 0x1000,
 * in the order it is written. A record may only refer to earlier
 * records, so a struct that points to itself is written in two steps:
 * a "forward reference" (only the name), used by the pointers, and
 * later the definition with the members. The debugger connects them by
 * the name. The basic types have fixed indices below 0x1000 (T_INT4 is
 * 0x74, a 64-bit pointer to it is 0x674) and need no record.
 *
 * Symbols (.debug$S):
 *     S_OBJNAME, S_COMPILE3                 what produced the module
 *     S_GPROC32 main ... S_END              one block per function, with
 *         S_FRAMEPROC                       how the frame is addressed
 *         S_REGREL32 argc  [rbp+16]         parameters, then locals
 *     S_GDATA32 counter                     global objects
 * Addresses are written as ".secrel32 label" (offset in its section)
 * and ".secidx label" (section number); the linker fills them.
 */
#include "codeview.h"
#include <stdlib.h>
#include <string.h>

/* type record kinds */
#define LF_POINTER    0x1002
#define LF_PROCEDURE  0x1008
#define LF_ARGLIST    0x1201
#define LF_FIELDLIST  0x1203
#define LF_BITFIELD   0x1205
#define LF_ARRAY      0x1503
#define LF_STRUCTURE  0x1505
#define LF_UNION      0x1506
#define LF_MEMBER     0x150d
#define LF_ULONG      0x8004
#define LF_UQUADWORD  0x800a

/* symbol record kinds */
#define S_END         0x0006
#define S_FRAMEPROC   0x1012
#define S_OBJNAME     0x1101
#define S_LDATA32     0x110c
#define S_GDATA32     0x110d
#define S_LPROC32     0x110f
#define S_GPROC32     0x1110
#define S_REGREL32    0x1111
#define S_COMPILE3    0x113c

/* basic types */
#define T_VOID        0x0003
#define T_RCHAR       0x0070
#define T_UCHAR       0x0020
#define T_SHORT       0x0011
#define T_USHORT      0x0021
#define T_LONG        0x0012
#define T_ULONG       0x0022
#define T_QUAD        0x0013
#define T_UQUAD       0x0023
#define T_INT4        0x0074
#define T_UINT4       0x0075
#define T_REAL32      0x0040
#define T_REAL64      0x0041
#define T_64PTR       0x0600     /* mode: 64-bit pointer to the basic type */

struct buf
{
    unsigned char* data;
    int size, capacity;
};

struct memo
{
    struct type* type;
    int index;
};

struct cv_reloc
{
    int pos;
    int is_secidx;           /* 2 bytes section number, else 4 bytes offset */
    const char* label;
};

struct codeview
{
    struct text* out;
    const char* files[256];
    int nfiles;

    struct buf types;        /* .debug$T */
    int next_index;          /* next type index */
    struct memo* memo;       /* types already written (pointers, arrays, functions, definitions) */
    int nmemo, memo_capacity;
    struct memo* fwd;        /* forward references of structs */
    int nfwd, fwd_capacity;

    struct buf syms;         /* .debug$S */
    struct cv_reloc* relocs;
    int nrelocs, reloc_capacity;
};

struct codeview* codeview_new(struct text* out)
{
    struct codeview* cv = cc_alloc(sizeof *cv);
    cv->out = out;
    cv->next_index = 0x1000;
    return cv;
}

int codeview_file(struct codeview* cv, const char* name)
{
    for (int i = 0; i < cv->nfiles; i++)
        if (strcmp(cv->files[i], name) == 0)
            return i + 1;
    if (cv->nfiles == 255)
        return 255;
    cv->files[cv->nfiles++] = name;

    /* Windows tools expect backslashes (cake writes "c:/dir/file.c") */
    char path[1024];
    snprintf(path, sizeof path, "%s", name);
    for (char* c = path; *c; c++)
        if (*c == '/') *c = '\\';
    name = path;
    text_printf(cv->out, "    .file %d \"%s\"\n", cv->nfiles, name);
    return cv->nfiles;
}

/* ---------------------------------------------------------------- bytes */

static void u8(struct buf* b, int v)
{
    if (b->size == b->capacity)
    {
        b->capacity = b->capacity * 2 + 1024;
        b->data = realloc(b->data, b->capacity);
        if (b->data == NULL)
        {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
    }
    b->data[b->size++] = (unsigned char)v;
}

static void u16(struct buf* b, unsigned v) { u8(b, (int)v); u8(b, (int)(v >> 8)); }
static void u32(struct buf* b, unsigned v) { for (int i = 0; i < 4; i++) u8(b, (int)(v >> (8 * i))); }

static void str(struct buf* b, const char* s)
{
    while (*s) u8(b, *s++);
    u8(b, 0);
}

/* numeric leaf: small values in 2 bytes, others with a prefix */
static void numeric(struct buf* b, unsigned long long v)
{
    if (v < 0x8000)
        u16(b, (unsigned)v);
    else if (v <= 0xFFFFFFFFull)
    {
        u16(b, LF_ULONG);
        u32(b, (unsigned)v);
    }
    else
    {
        u16(b, LF_UQUADWORD);
        u32(b, (unsigned)v);
        u32(b, (unsigned)(v >> 32));
    }
}

/* record: starts with length and kind; end_record pads and sets the length */
static int begin_record(struct buf* b, int kind)
{
    int start = b->size;
    u16(b, 0);
    u16(b, (unsigned)kind);
    return start;
}

static void end_record(struct buf* b, int start, int pad_with_lf)
{
    /* type records pad with LF_PAD bytes (0xF3 0xF2 0xF1), symbols with zeros */
    while ((b->size - start) % 4)
        u8(b, pad_with_lf ? 0xF0 + (4 - (b->size - start) % 4) : 0);
    int length = b->size - start - 2;
    b->data[start] = (unsigned char)length;
    b->data[start + 1] = (unsigned char)(length >> 8);
}

/* ---------------------------------------------------------------- types */

static int find_memo(struct memo* m, int n, struct type* t)
{
    for (int i = n - 1; i >= 0; i--)
        if (m[i].type == t)
            return m[i].index;
    return 0;
}

static void add_memo(struct memo** m, int* n, int* capacity, struct type* t, int index)
{
    if (*n == *capacity)
    {
        *capacity = *capacity * 2 + 64;
        *m = realloc(*m, sizeof(struct memo) * *capacity);
    }
    (*m)[*n].type = t;
    (*m)[*n].index = index;
    (*n)++;
}

static int basic_index(struct type* t)
{
    switch (t->kind)
    {
        case TY_VOID: return T_VOID;
        case TY_CHAR: return t->is_unsigned ? T_UCHAR : T_RCHAR;
        case TY_SHORT: return t->is_unsigned ? T_USHORT : T_SHORT;
        case TY_INT: return t->is_unsigned ? T_UINT4 : T_INT4;
        case TY_LONG: return t->is_unsigned ? T_ULONG : T_LONG;
        case TY_LLONG: return t->is_unsigned ? T_UQUAD : T_QUAD;
        case TY_FLOAT: return T_REAL32;
        case TY_DOUBLE:
        case TY_LDOUBLE: return T_REAL64;
        default: return 0;
    }
}

static int type_index(struct codeview* cv, struct type* t);

/* the name of a struct for the debugger; anonymous ones get a fixed name */
static const char* record_name(struct type* t)
{
    return t->tag ? t->tag : "<unnamed-tag>";
}

/* forward reference: a struct record with no members, only the name */
static int forward_ref(struct codeview* cv, struct type* t)
{
    int index = find_memo(cv->fwd, cv->nfwd, t);
    if (index)
        return index;
    struct buf* b = &cv->types;
    int start = begin_record(b, t->kind == TY_UNION ? LF_UNION : LF_STRUCTURE);
    u16(b, 0);               /* number of members */
    u16(b, 0x80);            /* property: forward reference */
    u32(b, 0);               /* field list */
    if (t->kind != TY_UNION)
    {
        u32(b, 0);           /* derived from */
        u32(b, 0);           /* vtable shape */
    }
    numeric(b, 0);
    str(b, record_name(t));
    end_record(b, start, 1);
    index = cv->next_index++;
    add_memo(&cv->fwd, &cv->nfwd, &cv->fwd_capacity, t, index);
    return index;
}

static int definition(struct codeview* cv, struct type* t)
{
    int index = find_memo(cv->memo, cv->nmemo, t);
    if (index)
        return index;

    /* the members' types first: a record only refers to earlier ones */
    int count = 0;
    for (struct member* m = t->members; m; m = m->next)
        if (m->name)
        {
            type_index(cv, m->type);
            count++;
        }

    /* a bit-field member has its own type record */
    int* bitfield_types = cc_alloc(sizeof(int) * (count + 1));
    int k = 0;
    for (struct member* m = t->members; m; m = m->next)
    {
        if (!m->name)
            continue;
        if (m->is_bitfield)
        {
            struct buf* b = &cv->types;
            int start = begin_record(b, LF_BITFIELD);
            u32(b, (unsigned)type_index(cv, m->type));
            u8(b, m->bit_width);
            u8(b, m->bit_offset);
            end_record(b, start, 1);
            bitfield_types[k] = cv->next_index++;
        }
        k++;
    }

    /* field list: one LF_MEMBER per member */
    struct buf* b = &cv->types;
    int start = begin_record(b, LF_FIELDLIST);
    k = 0;
    for (struct member* m = t->members; m; m = m->next)
    {
        if (!m->name)
            continue;
        u16(b, LF_MEMBER);
        u16(b, 3);           /* public */
        u32(b, (unsigned)(m->is_bitfield ? bitfield_types[k] : type_index(cv, m->type)));
        numeric(b, (unsigned)m->offset);
        str(b, m->name);
        while ((b->size - start) % 4)
            u8(b, 0xF0 + (4 - (b->size - start) % 4));
        k++;
    }
    end_record(b, start, 1);
    int fields = cv->next_index++;
    free(bitfield_types);

    start = begin_record(b, t->kind == TY_UNION ? LF_UNION : LF_STRUCTURE);
    u16(b, (unsigned)count);
    u16(b, 0);               /* property */
    u32(b, (unsigned)fields);
    if (t->kind != TY_UNION)
    {
        u32(b, 0);
        u32(b, 0);
    }
    numeric(b, (unsigned)t->size);
    str(b, record_name(t));
    end_record(b, start, 1);
    index = cv->next_index++;
    add_memo(&cv->memo, &cv->nmemo, &cv->memo_capacity, t, index);
    return index;
}

static int procedure(struct codeview* cv, struct type* ft)
{
    int args[256], n = 0;
    for (struct param* p = ft->params; p && n < 255; p = p->next)
        args[n++] = type_index(cv, p->type);
    if (ft->is_variadic)
        args[n++] = 0;       /* T_NOTYPE marks "..." */
    int ret = type_index(cv, ft->base);

    struct buf* b = &cv->types;
    int start = begin_record(b, LF_ARGLIST);
    u32(b, (unsigned)n);
    for (int i = 0; i < n; i++)
        u32(b, (unsigned)args[i]);
    end_record(b, start, 1);
    int arglist = cv->next_index++;

    start = begin_record(b, LF_PROCEDURE);
    u32(b, (unsigned)ret);
    u8(b, 0);                /* calling convention: near C */
    u8(b, 0);
    u16(b, (unsigned)n);
    u32(b, (unsigned)arglist);
    end_record(b, start, 1);
    return cv->next_index++;
}

static int type_index(struct codeview* cv, struct type* t)
{
    int basic = basic_index(t);
    if (basic)
        return basic;

    if (type_is_record(t))
    {
        if (t->is_complete)
            return definition(cv, t);
        return forward_ref(cv, t);
    }

    int index = find_memo(cv->memo, cv->nmemo, t);
    if (index)
        return index;

    struct buf* b = &cv->types;
    int start;
    switch (t->kind)
    {
        case TY_PTR:
        {
            int base = basic_index(t->base);
            if (base)
                return T_64PTR | base;
            /* a pointer to a named struct uses the forward reference, so
               a struct can point to itself */
            int target = type_is_record(t->base) && t->base->tag ? forward_ref(cv, t->base) : type_index(cv, t->base);
            start = begin_record(b, LF_POINTER);
            u32(b, (unsigned)target);
            u32(b, 0x0C | 8 << 13);     /* 64-bit pointer, size 8 */
            end_record(b, start, 1);
            index = cv->next_index++;
            break;
        }
        case TY_ARRAY:
        {
            int elem = type_index(cv, t->base);
            start = begin_record(b, LF_ARRAY);
            u32(b, (unsigned)elem);
            u32(b, T_UQUAD);            /* type of the index */
            numeric(b, (unsigned long long)(t->array_len < 0 ? 0 : t->size));
            str(b, "");
            end_record(b, start, 1);
            index = cv->next_index++;
            break;
        }
        case TY_FUNC:
            index = procedure(cv, t);
            break;
        default:
            return T_VOID;
    }
    add_memo(&cv->memo, &cv->nmemo, &cv->memo_capacity, t, index);
    return index;
}

/* -------------------------------------------------------------- symbols */

static void add_reloc(struct codeview* cv, int is_secidx, const char* label)
{
    if (cv->nrelocs == cv->reloc_capacity)
    {
        cv->reloc_capacity = cv->reloc_capacity * 2 + 64;
        cv->relocs = realloc(cv->relocs, sizeof(struct cv_reloc) * cv->reloc_capacity);
    }
    struct cv_reloc* r = &cv->relocs[cv->nrelocs++];
    r->pos = cv->syms.size;
    r->is_secidx = is_secidx;
    r->label = label;
    if (is_secidx) u16(&cv->syms, 0);
    else u32(&cv->syms, 0);
}

/* parameter or local: [frame register + offset] */
static void regrel(struct codeview* cv, struct obj* v, int frame_reg, int by_reference)
{
    struct buf* b = &cv->syms;
    int index = type_index(cv, v->type);
    if (by_reference)
    {
        /* Windows: a big struct parameter is a pointer to a copy */
        struct buf* tb = &cv->types;
        int start = begin_record(tb, LF_POINTER);
        u32(tb, (unsigned)index);
        u32(tb, 0x0C | 8 << 13);
        end_record(tb, start, 1);
        index = cv->next_index++;
    }
    int start = begin_record(b, S_REGREL32);
    u32(b, (unsigned)v->offset);
    u32(b, (unsigned)index);
    u16(b, (unsigned)frame_reg);
    str(b, v->name);
    end_record(b, start, 0);
}

static void function(struct codeview* cv, struct obj* fn, int frame_reg, int win64)
{
    struct buf* b = &cv->syms;
    int ftype = type_index(cv, fn->type);

    int start = begin_record(b, fn->is_static ? S_LPROC32 : S_GPROC32);
    u32(b, 0);               /* parent */
    int end_field = b->size;
    u32(b, 0);               /* end: offset of S_END, below */
    u32(b, 0);               /* next */
    u32(b, 0);               /* code size: the linker knows it */
    u32(b, 0);               /* debug start */
    u32(b, 0);               /* debug end */
    u32(b, (unsigned)ftype);
    add_reloc(cv, 0, fn->label);
    add_reloc(cv, 1, fn->label);
    u8(b, 0);                /* flags */
    str(b, fn->name);
    end_record(b, start, 0);

    start = begin_record(b, S_FRAMEPROC);
    for (int i = 0; i < 5; i++) u32(b, 0);
    u16(b, 0);
    u32(b, 2u << 14 | 2u << 16);   /* locals and parameters addressed by the frame pointer */
    end_record(b, start, 0);

    for (struct obj* v = fn->params; v; v = v->next)
    {
        int by_ref = win64 && type_is_record(v->type) &&
                     !(v->type->size == 1 || v->type->size == 2 || v->type->size == 4 || v->type->size == 8);
        regrel(cv, v, frame_reg, by_ref);
    }
    for (struct obj* v = fn->locals; v; v = v->next_local)
        if (v->param_index < 0)
            regrel(cv, v, frame_reg, 0);

    /* S_END; the function record points to it (offsets count from the
       start of the module stream, which has a 4-byte signature first) */
    int end_offset = b->size + 4;
    start = begin_record(b, S_END);
    end_record(b, start, 0);
    for (int i = 0; i < 4; i++)
        b->data[end_field + i] = (unsigned char)(end_offset >> (8 * i));
}

static void global(struct codeview* cv, struct obj* v)
{
    struct buf* b = &cv->syms;
    int index = type_index(cv, v->type);
    int start = begin_record(b, v->is_static ? S_LDATA32 : S_GDATA32);
    u32(b, (unsigned)index);
    add_reloc(cv, 0, v->label);
    add_reloc(cv, 1, v->label);
    str(b, v->name);
    end_record(b, start, 0);
}

/* ------------------------------------------------------------- output */

static void write_bytes(struct text* out, const unsigned char* p, int n)
{
    for (int i = 0; i < n; i += 16)
    {
        text_printf(out, "    .byte %d", p[i]);
        for (int j = i + 1; j < n && j < i + 16; j++)
            text_printf(out, ",%d", p[j]);
        text_printf(out, "\n");
    }
}

void codeview_emit(struct codeview* cv, struct program* prog, int frame_reg)
{
    struct buf* b = &cv->syms;
    int win64 = prog->target->kind == TARGET_WIN64;

    int start = begin_record(b, S_OBJNAME);
    u32(b, 0);
    str(b, cv->nfiles ? cv->files[0] : "");
    end_record(b, start, 0);

    start = begin_record(b, S_COMPILE3);
    u32(b, 0);               /* language: C */
    u16(b, 0xD0);            /* machine: x64 */
    for (int i = 0; i < 8; i++) u16(b, 0);   /* front-end and back-end versions */
    str(b, "cc89");
    end_record(b, start, 0);

    for (struct obj* v = prog->globals; v; v = v->next)
    {
        if (v->is_function && v->body)
            function(cv, v, frame_reg, win64);
        else if (!v->is_function && v->is_defined && v->name[0] != '.')
            global(cv, v);
    }

    text_printf(cv->out, "\n    .section .debug$T\n");
    write_bytes(cv->out, cv->types.data, cv->types.size);

    text_printf(cv->out, "\n    .section .debug$S\n");
    int at = 0;
    for (int i = 0; i < cv->nrelocs; i++)
    {
        struct cv_reloc* r = &cv->relocs[i];
        write_bytes(cv->out, b->data + at, r->pos - at);
        text_printf(cv->out, r->is_secidx ? "    .secidx %s\n" : "    .secrel32 %s\n", r->label);
        at = r->pos + (r->is_secidx ? 2 : 4);
    }
    write_bytes(cv->out, b->data + at, b->size - at);
}
