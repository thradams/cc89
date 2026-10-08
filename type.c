/*
 * Types. The sizes do not depend on the target: the parser chooses the
 * kind instead (spec 3.3). long is TY_LONG (4 bytes) on Windows and
 * TY_LLONG on LP64 targets; long double is TY_DOUBLE, except on
 * Linux x64 where it is TY_LDOUBLE (x87, 16 bytes).
 */
#include "type.h"
#include <string.h>

struct type* new_type(enum type_kind kind, int size, int align)
{
    struct type* t = cc_alloc(sizeof *t);
    t->kind = kind;
    t->size = size;
    t->align = align;
    t->array_len = -1;
    return t;
}

struct type* basic_type(enum type_kind kind, int is_unsigned)
{
    static const int sizes[] = {
        /* VOID CHAR SHORT INT LONG LLONG FLOAT DOUBLE LDOUBLE */
           1,   1,   2,    4,  4,   8,    4,    8,     16
    };
    struct type* t = new_type(kind, sizes[kind], sizes[kind]);
    t->is_unsigned = is_unsigned;
    return t;
}

struct type* pointer_to(struct type* base)
{
    struct type* t = new_type(TY_PTR, 8, 8);
    t->is_unsigned = 1;
    t->base = base;
    return t;
}

struct type* array_of(struct type* base, int len)
{
    struct type* t = new_type(TY_ARRAY, len < 0 ? 0 : base->size * len, base->align);
    t->base = base;
    t->array_len = len;
    return t;
}

struct type* func_type(struct type* ret)
{
    struct type* t = new_type(TY_FUNC, 1, 1);
    t->base = ret;
    return t;
}

int type_is_integer(const struct type* t)
{
    return t->kind >= TY_CHAR && t->kind <= TY_LLONG;
}

int type_is_float(const struct type* t)
{
    return t->kind >= TY_FLOAT && t->kind <= TY_LDOUBLE;
}

int type_is_arith(const struct type* t)
{
    return type_is_integer(t) || type_is_float(t);
}

int type_is_scalar(const struct type* t)
{
    return type_is_arith(t) || t->kind == TY_PTR;
}

int type_is_record(const struct type* t)
{
    return t->kind == TY_STRUCT || t->kind == TY_UNION;
}

/* spec 4.2: char and short become int (int holds all their values) */
struct type* promote(struct type* t)
{
    if (t->kind == TY_CHAR || t->kind == TY_SHORT)
        return basic_type(TY_INT, 0);
    return t;
}

/* spec 4.2: usual arithmetic conversions */
struct type* usual_arith(struct type* a, struct type* b)
{
    if (a->kind == TY_LDOUBLE || b->kind == TY_LDOUBLE) return basic_type(TY_LDOUBLE, 0);
    if (a->kind == TY_DOUBLE || b->kind == TY_DOUBLE) return basic_type(TY_DOUBLE, 0);
    if (a->kind == TY_FLOAT || b->kind == TY_FLOAT) return basic_type(TY_FLOAT, 0);

    a = promote(a);
    b = promote(b);
    if (a->kind == b->kind && a->is_unsigned == b->is_unsigned) return a;
    if (a->is_unsigned == b->is_unsigned) return a->kind > b->kind ? a : b;

    struct type* u = a->is_unsigned ? a : b;
    struct type* s = a->is_unsigned ? b : a;
    if (u->kind >= s->kind) return u;     /* rank of unsigned >= signed */
    if (s->size > u->size) return s;      /* signed holds every unsigned value */
    return basic_type(s->kind, 1);
}
