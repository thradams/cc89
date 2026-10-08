/*
 * Parser: tokens -> typed AST (spec 4 to 7).
 *
 * Recursive descent, one pass, semantic analysis at the same time.
 * The input is assumed to be valid C, so only the problems that block
 * code generation are reported.
 *
 * Every implicit conversion becomes an explicit N_CAST node and pointer
 * arithmetic is scaled here, so a back-end never needs to know C rules.
 */
#include "type.h"
#include <stdlib.h>
#include <string.h>

struct var_scope
{
    struct var_scope* next;
    const char* name;
    struct obj* var;
};

struct tag_scope
{
    struct tag_scope* next;
    const char* name;
    struct type* type;
};

struct scope
{
    struct scope* up;
    struct var_scope* vars;
    struct tag_scope* tags;
};

struct parser
{
    struct token* tok;
    struct scope* scope;
    struct program* prog;
    struct obj* globals_tail;
    struct obj* fn;              /* function being parsed */
    struct obj* locals_tail;
    int counter;                 /* unique names */
    int unit;                    /* translation unit number */
    int pack;                    /* #pragma pack, 0 = natural alignment */
    int pack_stack[32];
    int pack_depth;
    int selectany;               /* __declspec(selectany) seen in the specifiers */
    const struct target* target;
    struct type* va_list_type;   /* __builtin_va_list, created once */
};

enum storage { SC_NONE, SC_STATIC, SC_EXTERN, SC_AUTO, SC_REGISTER };

static struct node* expr(struct parser* p);
static struct node* assign(struct parser* p);
static struct node* cast_expr(struct parser* p);
static struct type* declspec(struct parser* p, enum storage* sc);
static struct type* declarator(struct parser* p, struct type* base, struct token** name);
static struct type* abstract_declarator(struct parser* p, struct type* base);
static struct stmt* compound_stmt(struct parser* p);

/* ------------------------------------------------------------- tokens */

static int is(struct parser* p, const char* s)
{
    struct token* t = p->tok;
    return (t->kind == TK_PUNCT || t->kind == TK_KEYWORD) && strcmp(t->text, s) == 0;
}

static int is_ident(struct parser* p, const char* s)
{
    return p->tok->kind == TK_IDENT && strcmp(p->tok->text, s) == 0;
}

static int accept(struct parser* p, const char* s)
{
    if (!is(p, s))
        return 0;
    p->tok++;
    return 1;
}

static void expect(struct parser* p, const char* s)
{
    if (!accept(p, s))
        cc_error_at(p->tok, "expected '%s' before '%s'", s, p->tok->text);
}

static struct token* expect_ident(struct parser* p)
{
    if (p->tok->kind != TK_IDENT)
        cc_error_at(p->tok, "expected identifier before '%s'", p->tok->text);
    return p->tok++;
}

/* Microsoft keywords that appear in cake output for Windows; ignored */
static int skip_ms_keywords(struct parser* p)
{
    static const char* names[] = {
        "__cdecl", "__stdcall", "__fastcall", "__vectorcall", "__inline",
        "__forceinline", "inline", "__ptr64", "__unaligned", "__restrict", 0
    };
    int skipped = 0;
    for (;;)
    {
        int found = 0;
        if (p->tok->kind == TK_IDENT)
        {
            for (int i = 0; names[i]; i++)
                if (strcmp(names[i], p->tok->text) == 0) found = 1;
            if (found)
                p->tok++;
            else if (strcmp(p->tok->text, "__declspec") == 0)
            {
                /* __declspec( ... ) */
                p->tok++;
                expect(p, "(");
                int depth = 1;
                while (depth > 0 && p->tok->kind != TK_EOF)
                {
                    if (is(p, "(")) depth++;
                    if (is(p, ")")) depth--;
                    if (is_ident(p, "selectany")) p->selectany = 1;
                    p->tok++;
                }
                found = 1;
            }
        }
        if (!found)
            return skipped;
        skipped = 1;
    }
}

/* ------------------------------------------------------------- scopes */

static void enter_scope(struct parser* p)
{
    struct scope* s = cc_alloc(sizeof *s);
    s->up = p->scope;
    p->scope = s;
}

static void leave_scope(struct parser* p)
{
    p->scope = p->scope->up;
}

static struct obj* find_var(struct parser* p, const char* name)
{
    for (struct scope* s = p->scope; s; s = s->up)
        for (struct var_scope* v = s->vars; v; v = v->next)
            if (strcmp(v->name, name) == 0)
                return v->var;
    return NULL;
}

static struct obj* find_var_in(struct scope* s, const char* name)
{
    for (struct var_scope* v = s->vars; v; v = v->next)
        if (strcmp(v->name, name) == 0)
            return v->var;
    return NULL;
}

static void push_var(struct scope* s, const char* name, struct obj* var)
{
    struct var_scope* v = cc_alloc(sizeof *v);
    v->name = name;
    v->var = var;
    v->next = s->vars;
    s->vars = v;
}

static struct type* find_tag(struct scope* s, const char* name, int all)
{
    for (; s; s = all ? s->up : NULL)
        for (struct tag_scope* t = s->tags; t; t = t->next)
            if (strcmp(t->name, name) == 0)
                return t->type;
    return NULL;
}

static void push_tag(struct scope* s, const char* name, struct type* type)
{
    struct tag_scope* t = cc_alloc(sizeof *t);
    t->name = name;
    t->type = type;
    t->next = s->tags;
    s->tags = t;
}

static struct scope* file_scope(struct parser* p)
{
    struct scope* s = p->scope;
    while (s->up) s = s->up;
    return s;
}

static void add_global(struct parser* p, struct obj* var)
{
    if (p->globals_tail)
        p->globals_tail->next = var;
    else
        p->prog->globals = var;
    p->globals_tail = var;
}

static struct obj* new_obj(const char* name, struct type* type)
{
    struct obj* var = cc_alloc(sizeof *var);
    var->name = name;
    var->label = name;
    var->type = type;
    var->param_index = -1;
    return var;
}

static struct obj* new_local(struct parser* p, const char* name, struct type* type)
{
    struct obj* var = new_obj(name, type);
    var->is_local = 1;
    if (p->locals_tail)
        p->locals_tail->next_local = var;
    else
        p->fn->locals = var;
    p->locals_tail = var;
    push_var(p->scope, name, var);
    return var;
}

/* objects with static duration that live inside a function (and strings) */
static struct obj* new_anon_static(struct parser* p, const char* name, struct type* type)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s.%d.%d", name, p->unit, p->counter++);
    struct obj* var = new_obj(name, type);
    var->label = cc_strndup(buf, (int)strlen(buf));
    var->is_static = 1;
    var->is_defined = 1;
    add_global(p, var);
    return var;
}

/* -------------------------------------------------------------- nodes */

static struct node* new_node(enum node_kind kind, const struct token* tok)
{
    struct node* n = cc_alloc(sizeof *n);
    n->kind = kind;
    n->tok = tok;
    return n;
}

static struct node* new_num(unsigned long long v, struct type* type, const struct token* tok)
{
    struct node* n = new_node(N_NUM, tok);
    n->ival = v;
    n->type = type;
    return n;
}

static struct node* new_binary(enum node_kind kind, struct node* lhs, struct node* rhs, struct type* type)
{
    struct node* n = new_node(kind, lhs->tok);
    n->lhs = lhs;
    n->rhs = rhs;
    n->type = type;
    return n;
}

static int same_scalar(struct type* a, struct type* b)
{
    if (a->kind != b->kind) return 0;
    if (a->kind == TY_PTR) return 1;
    return a->is_unsigned == b->is_unsigned;
}

static struct node* conv(struct node* n, struct type* type)
{
    if (type_is_record(type) || same_scalar(n->type, type))
        return n;
    struct node* c = new_node(N_CAST, n->tok);
    c->lhs = n;
    c->type = type;
    return c;
}

/* array -> pointer to the first element, function -> pointer to function */
static struct node* rv(struct node* n)
{
    if (n->type->kind == TY_ARRAY || n->type->kind == TY_FUNC)
    {
        struct type* t = n->type->kind == TY_ARRAY ? pointer_to(n->type->base) : pointer_to(n->type);
        if (n->kind == N_DEREF)       /* &*x is x */
        {
            struct node* x = n->lhs;
            if (x->type->kind != TY_PTR)
                return x;
            /* not conv: it keeps any pointer as it is, and here the
               pointed type changes (int (*)[3] -> int *) */
            struct node* c = new_node(N_CAST, x->tok);
            c->lhs = x;
            c->type = t;
            return c;
        }
        struct node* a = new_node(N_ADDR, n->tok);
        a->lhs = n;
        a->type = t;
        return a;
    }
    return n;
}

static int is_null_const(struct node* n)
{
    while (n->kind == N_CAST && (type_is_integer(n->type) || n->type->kind == TY_PTR))
        n = n->lhs;
    return n->kind == N_NUM && n->ival == 0;
}

/* ------------------------------------------------------- declarations */

static int is_typename(struct parser* p)
{
    static const char* kw[] = {
        "void", "char", "short", "int", "long", "float", "double", "signed",
        "unsigned", "struct", "union", "const", "volatile", "auto", "register",
        "static", "extern", 0
    };
    struct token* t = p->tok;
    if (t->kind == TK_KEYWORD)
    {
        for (int i = 0; kw[i]; i++)
            if (strcmp(kw[i], t->text) == 0) return 1;
        return 0;
    }
    if (t->kind == TK_IDENT)
        return strcmp(t->text, "__int64") == 0 || strcmp(t->text, "__declspec") == 0 ||
               strcmp(t->text, "__builtin_va_list") == 0 ||
               strcmp(t->text, "__inline") == 0 || strcmp(t->text, "__forceinline") == 0 ||
               strcmp(t->text, "inline") == 0;
    return 0;
}

static int align_to(int n, int align)
{
    return (n + align - 1) / align * align;
}

/*
 * gcc/clang layout (System V): bit-fields are packed one after the
 * other, whatever their types; a field only moves to the next unit
 * when it would cross a boundary of the size of its own type.
 * The struct takes the alignment of the types of its bit-fields.
 */
static void layout_record_gcc(struct parser* p, struct type* t)
{
    int bits = 0, size = 0, align = 1;

    for (struct member* m = t->members; m; m = m->next)
    {
        int ma = m->type->align;
        if (p->pack && ma > p->pack) ma = p->pack;

        if (t->kind == TY_UNION)
        {
            m->offset = 0;
            m->bit_offset = 0;
            if (m->type->size > size) size = m->type->size;
            if (ma > align) align = ma;
            continue;
        }

        if (m->is_bitfield)
        {
            int unit_bits = m->type->size * 8;
            if (m->bit_width == 0)
            {
                bits = align_to(bits, unit_bits);
                continue;
            }
            if (bits / unit_bits != (bits + m->bit_width - 1) / unit_bits)
                bits = align_to(bits, unit_bits);
            /* the unit is the aligned block of the type size that holds the field */
            m->offset = bits / unit_bits * m->type->size;
            m->bit_offset = bits - m->offset * 8;
            bits += m->bit_width;
            if (m->name && ma > align) align = ma;
            continue;
        }

        int offset = align_to(align_to(bits, 8) / 8, ma);
        m->offset = offset;
        bits = (offset + m->type->size) * 8;
        if (ma > align) align = ma;
    }

    if (t->kind == TY_STRUCT)
        size = align_to(bits, 8) / 8;
    t->align = align;
    t->size = align_to(size, align);
    t->is_complete = 1;
}

/*
 * Layout follows MSVC, so structs match the Windows headers:
 * a bit-field shares the storage unit of the previous one when both
 * have the same unit size and the bits fit.
 */
static void layout_record(struct parser* p, struct type* t)
{
    if (p->target->gcc_bitfields)
    {
        layout_record_gcc(p, t);
        return;
    }

    int offset = 0, size = 0, align = 1;
    int unit_size = 0, unit_offset = 0, bits_used = 0;

    for (struct member* m = t->members; m; m = m->next)
    {
        int ma = m->type->align;
        if (p->pack && ma > p->pack) ma = p->pack;

        if (t->kind == TY_UNION)
        {
            m->offset = 0;
            m->bit_offset = 0;
            if (m->type->size > size) size = m->type->size;
            if (ma > align) align = ma;
            continue;
        }

        if (m->is_bitfield)
        {
            if (m->bit_width == 0)
            {
                unit_size = 0;          /* close the current unit */
                continue;
            }
            if (unit_size == m->type->size && bits_used + m->bit_width <= unit_size * 8)
            {
                m->offset = unit_offset;
                m->bit_offset = bits_used;
                bits_used += m->bit_width;
                continue;
            }
            offset = align_to(offset, ma);
            unit_size = m->type->size;
            unit_offset = offset;
            m->offset = offset;
            m->bit_offset = 0;
            bits_used = m->bit_width;
            offset += unit_size;
            if (ma > align) align = ma;
            continue;
        }

        unit_size = 0;
        offset = align_to(offset, ma);
        m->offset = offset;
        offset += m->type->size;
        if (ma > align) align = ma;
    }

    if (t->kind == TY_STRUCT)
        size = offset;
    t->align = align;
    t->size = align_to(size, align);
    t->is_complete = 1;
}

static long long const_int(struct parser* p);

static struct type* struct_decl(struct parser* p)
{
    enum type_kind kind = strcmp(p->tok->text, "struct") == 0 ? TY_STRUCT : TY_UNION;
    p->tok++;
    skip_ms_keywords(p);

    const char* tag = NULL;
    if (p->tok->kind == TK_IDENT)
        tag = (p->tok++)->text;

    if (tag && !is(p, "{"))
    {
        struct type* t = find_tag(p->scope, tag, 1);
        if (t)
            return t;
        t = new_type(kind, 0, 1);
        t->tag = tag;
        push_tag(p->scope, tag, t);
        return t;
    }

    expect(p, "{");

    /* complete a forward declaration of the same scope, or make a new type */
    struct type* t = tag ? find_tag(p->scope, tag, 0) : NULL;
    if (t == NULL || t->is_complete)
    {
        t = new_type(kind, 0, 1);
        t->tag = tag;
        if (tag)
            push_tag(p->scope, tag, t);
    }

    struct member head = { 0 };
    struct member* cur = &head;
    while (!accept(p, "}"))
    {
        if (p->tok->kind == TK_PRAGMA)
        {
            p->tok++;
            continue;
        }
        struct type* base = declspec(p, NULL);
        int first = 1;
        while (!accept(p, ";"))
        {
            if (!first)
                expect(p, ",");
            first = 0;

            struct member* m = cc_alloc(sizeof *m);
            struct token* name = NULL;
            m->type = is(p, ":") ? base : declarator(p, base, &name);
            m->name = name ? name->text : NULL;
            if (accept(p, ":"))
            {
                m->is_bitfield = 1;
                m->bit_width = (int)const_int(p);
            }
            cur = cur->next = m;
        }
    }
    t->members = head.next;
    layout_record(p, t);
    return t;
}

/*
 * __builtin_va_list
 *   System V x64: struct { unsigned gp_offset, fp_offset;
 *                          void* overflow_arg_area; void* reg_save_area; }[1]
 *   AAPCS64 (Linux arm64): struct { void* __stack; void* __gr_top; void* __vr_top;
 *                                   int __gr_offs; int __vr_offs; }
 *   others: char*
 */
static struct type* va_list_type(struct parser* p)
{
    if (p->va_list_type)
        return p->va_list_type;
    if (p->target->va_list_kind == VA_CHAR_PTR)
        return p->va_list_type = pointer_to(basic_type(TY_CHAR, 0));

    if (p->target->va_list_kind == VA_AAPCS)
    {
        struct type* t = new_type(TY_STRUCT, 0, 1);
        const char* names[] = { "__stack", "__gr_top", "__vr_top", "__gr_offs", "__vr_offs" };
        struct member head = { 0 };
        struct member* cur = &head;
        for (int i = 0; i < 5; i++)
        {
            cur = cur->next = cc_alloc(sizeof(struct member));
            cur->name = names[i];
            cur->type = i < 3 ? pointer_to(basic_type(TY_VOID, 0)) : basic_type(TY_INT, 0);
        }
        t->members = head.next;
        t->tag = "__va_list";
        layout_record(p, t);
        return p->va_list_type = t;
    }

    struct type* t = new_type(TY_STRUCT, 0, 1);
    const char* names[] = { "gp_offset", "fp_offset", "overflow_arg_area", "reg_save_area" };
    struct member head = { 0 };
    struct member* cur = &head;
    for (int i = 0; i < 4; i++)
    {
        cur = cur->next = cc_alloc(sizeof(struct member));
        cur->name = names[i];
        cur->type = i < 2 ? basic_type(TY_INT, 1) : pointer_to(basic_type(TY_VOID, 0));
    }
    t->members = head.next;
    layout_record(p, t);
    return p->va_list_type = array_of(t, 1);
}

/* spec 3.1: combinations of type specifiers */
static struct type* declspec(struct parser* p, enum storage* sc)
{
    int n_void = 0, n_char = 0, n_short = 0, n_int = 0, n_long = 0;
    int n_float = 0, n_double = 0, n_signed = 0, n_unsigned = 0;
    struct type* record = NULL;
    struct token* start = p->tok;

    if (sc)
        *sc = SC_NONE;

    for (;;)
    {
        if (skip_ms_keywords(p))
            continue;
        if (is_ident(p, "__int64")) { p->tok++; n_long += 2; continue; }
        if (is_ident(p, "__builtin_va_list")) { p->tok++; record = va_list_type(p); continue; }
        if (p->tok->kind != TK_KEYWORD)
            break;

        const char* s = p->tok->text;
        if (!strcmp(s, "const") || !strcmp(s, "volatile")) { p->tok++; continue; }
        if (!strcmp(s, "static") || !strcmp(s, "extern") ||
            !strcmp(s, "auto") || !strcmp(s, "register"))
        {
            if (sc == NULL)
                cc_error_at(p->tok, "storage class not allowed here");
            *sc = s[0] == 's' ? SC_STATIC : s[0] == 'e' ? SC_EXTERN : s[0] == 'a' ? SC_AUTO : SC_REGISTER;
            p->tok++;
            continue;
        }
        if (!strcmp(s, "struct") || !strcmp(s, "union")) { record = struct_decl(p); continue; }
        if (!strcmp(s, "void")) n_void++;
        else if (!strcmp(s, "char")) n_char++;
        else if (!strcmp(s, "short")) n_short++;
        else if (!strcmp(s, "int")) n_int++;
        else if (!strcmp(s, "long")) n_long++;
        else if (!strcmp(s, "float")) n_float++;
        else if (!strcmp(s, "double")) n_double++;
        else if (!strcmp(s, "signed")) n_signed++;
        else if (!strcmp(s, "unsigned")) n_unsigned++;
        else break;
        p->tok++;
    }

    if (record) return record;
    if (n_void) return basic_type(TY_VOID, 0);
    if (n_float) return basic_type(TY_FLOAT, 0);
    if (n_double)
        return basic_type(n_long && p->target->long_double_size == 16 ? TY_LDOUBLE : TY_DOUBLE, 0);
    if (n_char) return basic_type(TY_CHAR, n_unsigned > 0 || (!n_signed && p->target->char_unsigned));
    if (n_short) return basic_type(TY_SHORT, n_unsigned > 0);
    if (n_long >= 2) return basic_type(TY_LLONG, n_unsigned > 0);
    /* LP64: long has the size and behavior of long long */
    if (n_long) return basic_type(p->target->long_size == 8 ? TY_LLONG : TY_LONG, n_unsigned > 0);
    if (!n_int && !n_signed && !n_unsigned && p->tok == start)
        cc_error_at(p->tok, "expected a type before '%s'", p->tok->text);
    return basic_type(TY_INT, n_unsigned > 0);
}

static struct type* pointers(struct parser* p, struct type* t)
{
    for (;;)
    {
        if (skip_ms_keywords(p)) continue;
        if (accept(p, "const") || accept(p, "volatile")) continue;
        if (accept(p, "*")) { t = pointer_to(t); continue; }
        return t;
    }
}

static struct type* params(struct parser* p, struct type* ret)
{
    struct type* fn = func_type(ret);

    if (is(p, "void") && p->tok[1].kind == TK_PUNCT && strcmp(p->tok[1].text, ")") == 0)
    {
        p->tok += 2;
        fn->has_prototype = 1;
        return fn;
    }
    if (accept(p, ")"))
        return fn;        /* () : no prototype */

    if (!is_typename(p))
        cc_error_at(p->tok, "K&R function definitions are not supported");

    fn->has_prototype = 1;
    struct param head = { 0 };
    struct param* cur = &head;
    for (;;)
    {
        if (accept(p, "..."))
        {
            fn->is_variadic = 1;
            expect(p, ")");
            break;
        }
        struct token* name = NULL;
        struct type* t = declarator(p, declspec(p, (enum storage[]){ 0 }), &name);
        if (t->kind == TY_ARRAY) t = pointer_to(t->base);
        if (t->kind == TY_FUNC) t = pointer_to(t);
        cur = cur->next = cc_alloc(sizeof(struct param));
        cur->type = t;
        cur->name = name ? name->text : NULL;
        cur->tok = name;
        if (accept(p, ")"))
            break;
        expect(p, ",");
    }
    fn->params = head.next;
    return fn;
}

static struct type* type_suffix(struct parser* p, struct type* t)
{
    if (accept(p, "["))
    {
        int len = -1;
        if (!is(p, "]"))
            len = (int)const_int(p);
        expect(p, "]");
        t = type_suffix(p, t);
        return array_of(t, len);
    }
    if (accept(p, "("))
    {
        struct type* fn = params(p, NULL);
        t = type_suffix(p, t);
        fn->base = t;
        return fn;
    }
    return t;
}

/* does '(' start a parameter list (abstract declarators)? */
static int paren_is_params(struct parser* p)
{
    struct token* next = p->tok + 1;
    if (next->kind == TK_PUNCT && strcmp(next->text, ")") == 0)
        return 1;
    struct token* save = p->tok;
    p->tok = next;
    int r = is_typename(p);
    p->tok = save;
    return r;
}

/*
 * declarator: pointer? ( identifier | ( declarator ) ) suffix*
 * The nested declarator applies to the type after the suffix, so it is
 * parsed twice: first skipped, then parsed again with the final type.
 * name may be NULL in abstract declarators.
 */
static struct type* declarator(struct parser* p, struct type* t, struct token** name)
{
    t = pointers(p, t);
    skip_ms_keywords(p);

    if (is(p, "(") && !paren_is_params(p))
    {
        struct token* start = p->tok;
        p->tok++;
        struct type dummy = { 0 };
        declarator(p, &dummy, name);
        expect(p, ")");
        t = type_suffix(p, t);
        struct token* end = p->tok;
        p->tok = start + 1;
        t = declarator(p, t, name);
        p->tok = end;
        return t;
    }

    if (p->tok->kind == TK_IDENT && name)
        *name = p->tok++;
    return type_suffix(p, t);
}

static struct type* abstract_declarator(struct parser* p, struct type* t)
{
    struct token* name = NULL;
    t = declarator(p, t, &name);
    if (name)
        cc_error_at(name, "unexpected identifier in type name");
    return t;
}

static struct type* type_name(struct parser* p)
{
    return abstract_declarator(p, declspec(p, NULL));
}

/* -------------------------------------------------------- expressions */

static struct node* new_add(struct node* lhs, struct node* rhs, const struct token* tok)
{
    lhs = rv(lhs);
    rhs = rv(rhs);

    if (type_is_arith(lhs->type) && type_is_arith(rhs->type))
    {
        struct type* t = usual_arith(lhs->type, rhs->type);
        return new_binary(N_ADD, conv(lhs, t), conv(rhs, t), t);
    }
    if (lhs->type->kind != TY_PTR)
    {
        struct node* tmp = lhs; lhs = rhs; rhs = tmp;
    }
    if (lhs->type->kind != TY_PTR || !type_is_integer(rhs->type))
        cc_error_at(tok, "invalid operands to '+'");

    /* ptr + n  is  ptr + n * sizeof *ptr */
    struct type* ll = basic_type(TY_LLONG, 0);
    struct node* scaled = new_binary(N_MUL, conv(rhs, ll), new_num(lhs->type->base->size, ll, tok), ll);
    return new_binary(N_ADD, lhs, scaled, lhs->type);
}

static struct node* new_sub(struct node* lhs, struct node* rhs, const struct token* tok)
{
    lhs = rv(lhs);
    rhs = rv(rhs);
    struct type* ll = basic_type(TY_LLONG, 0);

    if (type_is_arith(lhs->type) && type_is_arith(rhs->type))
    {
        struct type* t = usual_arith(lhs->type, rhs->type);
        return new_binary(N_SUB, conv(lhs, t), conv(rhs, t), t);
    }
    if (lhs->type->kind == TY_PTR && type_is_integer(rhs->type))
    {
        struct node* scaled = new_binary(N_MUL, conv(rhs, ll), new_num(lhs->type->base->size, ll, tok), ll);
        return new_binary(N_SUB, lhs, scaled, lhs->type);
    }
    if (lhs->type->kind == TY_PTR && rhs->type->kind == TY_PTR)
    {
        /* (p - q) / sizeof *p, type ptrdiff_t (long long on Windows) */
        struct node* diff = new_binary(N_SUB, conv(lhs, ll), conv(rhs, ll), ll);
        int size = lhs->type->base->size;
        return size == 1 ? diff : new_binary(N_DIV, diff, new_num(size, ll, tok), ll);
    }
    cc_error_at(tok, "invalid operands to '-'");
    return NULL;
}

static struct node* new_arith(enum node_kind kind, struct node* lhs, struct node* rhs)
{
    lhs = rv(lhs);
    rhs = rv(rhs);
    if (kind == N_SHL || kind == N_SHR)
    {
        struct type* t = promote(lhs->type);
        return new_binary(kind, conv(lhs, t), conv(rhs, basic_type(TY_INT, 0)), t);
    }
    if (!type_is_arith(lhs->type) || !type_is_arith(rhs->type))
        cc_error_at(lhs->tok, "invalid operands to binary operator");
    struct type* t = usual_arith(lhs->type, rhs->type);
    return new_binary(kind, conv(lhs, t), conv(rhs, t), t);
}

static struct node* new_compare(enum node_kind kind, struct node* lhs, struct node* rhs)
{
    lhs = rv(lhs);
    rhs = rv(rhs);
    if (type_is_arith(lhs->type) && type_is_arith(rhs->type))
    {
        struct type* t = usual_arith(lhs->type, rhs->type);
        lhs = conv(lhs, t);
        rhs = conv(rhs, t);
    }
    else
    {
        /* pointers (or pointer and null constant) compare as unsigned 64 bits */
        struct type* t = lhs->type->kind == TY_PTR ? lhs->type : rhs->type;
        lhs = conv(lhs, t);
        rhs = conv(rhs, t);
    }
    return new_binary(kind, lhs, rhs, basic_type(TY_INT, 0));
}

static struct node* new_assign_op(enum node_kind op, struct node* lhs, struct node* rhs, int is_post, const struct token* tok)
{
    struct node* n = new_node(N_ASSIGN_OP, tok);
    n->lhs = lhs;
    n->op = op;
    n->is_post = is_post;
    n->type = lhs->type;
    rhs = rv(rhs);

    if (lhs->type->kind == TY_PTR)
    {
        struct type* ll = basic_type(TY_LLONG, 0);
        n->op_type = lhs->type;
        n->rhs = new_binary(N_MUL, conv(rhs, ll), new_num(lhs->type->base->size, ll, tok), ll);
    }
    else if (op == N_SHL || op == N_SHR)
    {
        n->op_type = promote(lhs->type);
        n->rhs = conv(rhs, basic_type(TY_INT, 0));
    }
    else
    {
        n->op_type = usual_arith(lhs->type, rhs->type);
        n->rhs = conv(rhs, n->op_type);
    }
    return n;
}

/* L"...": UTF-8 bytes -> UTF-16 units (wchar_t is unsigned short on Windows) */
static int utf8_to_utf16(const unsigned char* s, int n, unsigned short* out)
{
    int len = 0;
    for (int i = 0; i < n;)
    {
        unsigned c = s[i++];
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) c &= 0x3F >> extra;
        for (; extra > 0 && i < n; extra--)
            c = c << 6 | (s[i++] & 0x3F);
        if (c > 0xFFFF)
        {
            c -= 0x10000;
            if (out) { out[len] = (unsigned short)(0xD800 | c >> 10); out[len + 1] = (unsigned short)(0xDC00 | (c & 0x3FF)); }
            len += 2;
        }
        else
        {
            if (out) out[len] = (unsigned short)c;
            len++;
        }
    }
    return len;
}

static struct node* new_string(struct parser* p, struct token* tok)
{
    if (tok->is_wide && p->target->wchar_size == 4)
    {
        /* wchar_t is int: one UTF-32 code point per element */
        const unsigned char* s = (const unsigned char*)tok->str;
        int len = 0;
        unsigned* units = cc_alloc(sizeof(unsigned) * (tok->str_len + 1));
        for (int i = 0; i < tok->str_len;)
        {
            unsigned c = s[i++];
            int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
            if (extra) c &= 0x3F >> extra;
            for (; extra > 0 && i < tok->str_len; extra--)
                c = c << 6 | (s[i++] & 0x3F);
            units[len++] = c;
        }
        struct type* t = array_of(basic_type(TY_INT, 0), len + 1);
        struct obj* var = new_anon_static(p, ".str", t);
        var->data = cc_alloc(t->size);
        for (int i = 0; i < len; i++)
            for (int b = 0; b < 4; b++)
                var->data[4 * i + b] = (unsigned char)(units[i] >> (8 * b));
        struct node* n = new_node(N_VAR, tok);
        n->var = var;
        n->type = t;
        return n;
    }

    if (tok->is_wide)
    {
        int len = utf8_to_utf16((const unsigned char*)tok->str, tok->str_len, NULL);
        struct type* t = array_of(basic_type(TY_SHORT, 1), len + 1);
        struct obj* var = new_anon_static(p, ".str", t);
        unsigned short* units = cc_alloc(sizeof(unsigned short) * (len + 1));
        utf8_to_utf16((const unsigned char*)tok->str, tok->str_len, units);
        var->data = cc_alloc(t->size);
        for (int i = 0; i < len; i++)
        {
            var->data[2 * i] = (unsigned char)units[i];
            var->data[2 * i + 1] = (unsigned char)(units[i] >> 8);
        }
        struct node* n = new_node(N_VAR, tok);
        n->var = var;
        n->type = t;
        return n;
    }

    struct type* t = array_of(basic_type(TY_CHAR, 0), tok->str_len + 1);
    struct obj* var = new_anon_static(p, ".str", t);
    var->data = cc_alloc(tok->str_len + 1);
    memcpy(var->data, tok->str, tok->str_len);
    struct node* n = new_node(N_VAR, tok);
    n->var = var;
    n->type = t;
    return n;
}

static struct node* funcall(struct parser* p, struct node* fn)
{
    struct token* tok = p->tok - 1;  /* ( */

    struct node* args[256];
    int nargs = 0;
    while (!accept(p, ")"))
    {
        if (nargs)
            expect(p, ",");
        if (nargs == 256)
            cc_error_at(p->tok, "too many arguments");
        args[nargs++] = rv(assign(p));
    }

    /* __va_start(&ap, last) is the MSVC va_start used by cake output */
    if (fn->kind == N_VAR && strcmp(fn->var->name, "__va_start") == 0)
    {
        if (nargs != 2)
            cc_error_at(tok, "__va_start requires 2 arguments");
        struct node* n = new_node(N_VA_START, tok);
        n->lhs = args[0];
        n->rhs = args[1]->kind == N_CAST ? args[1]->lhs : args[1];
        if (n->rhs->kind != N_VAR || n->rhs->var->param_index < 0)
            cc_error_at(tok, "the second argument of __va_start must be a parameter");
        n->type = basic_type(TY_VOID, 0);
        return n;
    }

    fn = rv(fn);
    if (fn->type->kind != TY_PTR || fn->type->base->kind != TY_FUNC)
        cc_error_at(tok, "called object is not a function");
    struct type* ft = fn->type->base;

    struct param* param = ft->params;
    for (int i = 0; i < nargs; i++)
    {
        if (param)
        {
            args[i] = conv(args[i], param->type);
            param = param->next;
        }
        else
        {
            /* default argument promotions */
            struct type* t = args[i]->type;
            if (t->kind == TY_FLOAT)
                args[i] = conv(args[i], basic_type(TY_DOUBLE, 0));
            else if (type_is_integer(t))
                args[i] = conv(args[i], promote(t));
        }
    }

    struct node* n = new_node(N_CALL, tok);
    n->lhs = fn;
    n->type = ft->base;
    n->nargs = nargs;
    n->args = cc_alloc(sizeof(struct node*) * (nargs + 1));
    memcpy(n->args, args, sizeof(struct node*) * nargs);
    return n;
}

static struct member* find_member(struct type* t, const char* name)
{
    for (struct member* m = t->members; m; m = m->next)
        if (m->name && strcmp(m->name, name) == 0)
            return m;
    return NULL;
}

static struct node* member_access(struct node* lhs, struct token* name)
{
    if (!type_is_record(lhs->type) || !lhs->type->is_complete)
        cc_error_at(name, "member access on a type that is not a complete struct or union");
    struct member* m = find_member(lhs->type, name->text);
    if (m == NULL)
        cc_error_at(name, "no member named '%s'", name->text);
    struct node* n = new_node(N_MEMBER, name);
    n->lhs = lhs;
    n->member = m;
    n->type = m->type;
    return n;
}

/*
 * Address of the va_list object.
 *   System V: va_list is an array of one struct. A local decays to the
 *   address of the struct; a parameter already is that pointer.
 *   Others (char* or the AAPCS64 struct): &ap.
 */
static struct node* va_list_address(struct parser* p, struct node* ap)
{
    if (p->target->va_list_kind == VA_SYSV)
        return rv(ap);
    struct node* n = new_node(N_ADDR, ap->tok);
    n->lhs = ap;
    n->type = pointer_to(ap->type);
    return n;
}

/*
 * gcc builtins found in cake output for gcc/clang targets.
 * Returns NULL when the name is not a builtin handled here.
 */
static struct node* builtin(struct parser* p, struct token* tok)
{
    const char* name = tok->text;
    if (strncmp(name, "__builtin_", 10) != 0 || !is(p, "("))
        return NULL;
    name += 10;

    if (!strcmp(name, "va_start") || !strcmp(name, "va_arg") ||
        !strcmp(name, "va_end") || !strcmp(name, "va_copy"))
    {
        expect(p, "(");
        struct node* ap = assign(p);
        struct node* n;
        if (!strcmp(name, "va_start"))
        {
            expect(p, ",");
            struct node* last = assign(p);
            if (last->kind != N_VAR || last->var->param_index < 0)
                cc_error_at(tok, "the second argument of va_start must be a parameter");
            n = new_node(N_VA_START, tok);
            n->lhs = va_list_address(p, ap);
            n->rhs = last;
            n->type = basic_type(TY_VOID, 0);
        }
        else if (!strcmp(name, "va_arg"))
        {
            expect(p, ",");
            n = new_node(N_VA_ARG, tok);
            n->lhs = va_list_address(p, ap);
            n->type = type_name(p);
        }
        else if (!strcmp(name, "va_copy"))
        {
            expect(p, ",");
            struct node* src = assign(p);
            struct node* d = new_node(N_DEREF, tok);
            d->lhs = va_list_address(p, ap);
            d->type = d->lhs->type->base;
            struct node* s = new_node(N_DEREF, tok);
            s->lhs = va_list_address(p, src);
            s->type = s->lhs->type->base;
            n = new_binary(N_ASSIGN, d, s, d->type);
        }
        else
        {
            /* va_end does nothing */
            n = new_node(N_CAST, tok);
            n->lhs = rv(ap);
            n->type = basic_type(TY_VOID, 0);
        }
        expect(p, ")");
        return n;
    }

    /* infinity and NaN constants */
    if (!strcmp(name, "inf") || !strcmp(name, "inff") || !strcmp(name, "infl") ||
        !strcmp(name, "nan") || !strcmp(name, "nanf") || !strcmp(name, "nanl") ||
        !strcmp(name, "huge_val") || !strcmp(name, "huge_valf"))
    {
        expect(p, "(");
        if (p->tok->kind == TK_STRING) p->tok++;
        expect(p, ")");
        struct node* n = new_node(N_FNUM, tok);
        n->fval = name[0] == 'n' ? strtod("nan", NULL) : strtod("inf", NULL);
        n->type = basic_type(name[strlen(name) - 1] == 'f' ? TY_FLOAT : TY_DOUBLE, 0);
        return n;
    }

    /* __builtin_floor(x) is floor(x): a call to the C library.
       Declared as T name(T) with T float for the names ending in f. */
    struct obj* var = find_var_in(file_scope(p), name);
    if (var == NULL)
    {
        struct type* t = basic_type(name[strlen(name) - 1] == 'f' ? TY_FLOAT : TY_DOUBLE, 0);
        struct type* ft = func_type(t);
        ft->has_prototype = 1;
        ft->params = cc_alloc(sizeof(struct param));
        ft->params->type = t;
        var = new_obj(name, ft);
        var->is_function = 1;
        push_var(file_scope(p), name, var);
        add_global(p, var);
    }
    struct node* n = new_node(N_VAR, tok);
    n->var = var;
    n->type = var->type;
    return n;
}

static struct node* primary(struct parser* p)
{
    struct token* tok = p->tok;

    if (accept(p, "("))
    {
        struct node* n = expr(p);
        expect(p, ")");
        return n;
    }

    if (tok->kind == TK_IDENT)
    {
        p->tok++;
        struct node* b = builtin(p, tok);
        if (b)
            return b;
        p->tok--;
        p->tok++;
        struct obj* var = find_var(p, tok->text);
        if (var == NULL)
        {
            if (!is(p, "("))
                cc_error_at(tok, "'%s' undeclared", tok->text);
            /* implicit declaration: extern int name(); */
            cc_warning(tok->file, tok->line, tok->col, "implicit declaration of function '%s'", tok->text);
            var = new_obj(tok->text, func_type(basic_type(TY_INT, 0)));
            var->is_function = 1;
            push_var(file_scope(p), tok->text, var);
            add_global(p, var);
        }
        struct node* n = new_node(N_VAR, tok);
        n->var = var;
        n->type = var->type;
        return n;
    }

    if (tok->kind == TK_INT)
    {
        p->tok++;
        struct type* t = basic_type(tok->is_long_long ? TY_LLONG : TY_INT, tok->is_unsigned);
        return new_num(tok->ival, t, tok);
    }

    if (tok->kind == TK_FLOAT)
    {
        p->tok++;
        struct node* n = new_node(N_FNUM, tok);
        n->fval = tok->fval;
        n->type = basic_type(tok->is_float ? TY_FLOAT : TY_DOUBLE, 0);
        return n;
    }

    if (tok->kind == TK_STRING)
    {
        p->tok++;
        return new_string(p, tok);
    }

    cc_error_at(tok, "expected expression before '%s'", tok->text);
    return NULL;
}

static struct node* postfix(struct parser* p)
{
    struct node* n = primary(p);
    for (;;)
    {
        struct token* tok = p->tok;
        if (accept(p, "["))
        {
            struct node* idx = expr(p);
            expect(p, "]");
            struct node* sum = new_add(n, idx, tok);
            n = new_node(N_DEREF, tok);
            n->lhs = sum;
            n->type = sum->type->base;
            continue;
        }
        if (accept(p, "("))
        {
            n = funcall(p, n);
            continue;
        }
        if (accept(p, "."))
        {
            n = member_access(n, expect_ident(p));
            continue;
        }
        if (accept(p, "->"))
        {
            n = rv(n);
            if (n->type->kind != TY_PTR)
                cc_error_at(tok, "'->' on a value that is not a pointer");
            struct node* d = new_node(N_DEREF, tok);
            d->lhs = n;
            d->type = n->type->base;
            n = member_access(d, expect_ident(p));
            continue;
        }
        if (accept(p, "++"))
        {
            n = new_assign_op(N_ADD, n, new_num(1, basic_type(TY_INT, 0), tok), 1, tok);
            continue;
        }
        if (accept(p, "--"))
        {
            n = new_assign_op(N_SUB, n, new_num(1, basic_type(TY_INT, 0), tok), 1, tok);
            continue;
        }
        return n;
    }
}

static struct node* unary(struct parser* p)
{
    struct token* tok = p->tok;

    if (accept(p, "++"))
        return new_assign_op(N_ADD, unary(p), new_num(1, basic_type(TY_INT, 0), tok), 0, tok);
    if (accept(p, "--"))
        return new_assign_op(N_SUB, unary(p), new_num(1, basic_type(TY_INT, 0), tok), 0, tok);

    if (accept(p, "&"))
    {
        struct node* operand = cast_expr(p);
        if (operand->kind == N_DEREF)      /* &*x is x */
            return rv(operand->lhs);
        struct node* n = new_node(N_ADDR, tok);
        n->lhs = operand;
        n->type = pointer_to(operand->type);
        return n;
    }
    if (accept(p, "*"))
    {
        struct node* operand = rv(cast_expr(p));
        if (operand->type->kind != TY_PTR)
            cc_error_at(tok, "'*' on a value that is not a pointer");
        struct node* n = new_node(N_DEREF, tok);
        n->lhs = operand;
        n->type = operand->type->base;
        return n;
    }
    if (accept(p, "+"))
    {
        struct node* operand = rv(cast_expr(p));
        return conv(operand, promote(operand->type));
    }
    if (accept(p, "-"))
    {
        struct node* operand = rv(cast_expr(p));
        struct type* t = promote(operand->type);
        struct node* n = new_node(N_NEG, tok);
        n->lhs = conv(operand, t);
        n->type = t;
        return n;
    }
    if (accept(p, "~"))
    {
        struct node* operand = rv(cast_expr(p));
        struct type* t = promote(operand->type);
        struct node* n = new_node(N_BITNOT, tok);
        n->lhs = conv(operand, t);
        n->type = t;
        return n;
    }
    if (accept(p, "!"))
    {
        struct node* n = new_node(N_NOT, tok);
        n->lhs = rv(cast_expr(p));
        n->type = basic_type(TY_INT, 0);
        return n;
    }
    if (accept(p, "sizeof"))
    {
        struct type* t;
        if (is(p, "(") && (p->tok++, is_typename(p)))
        {
            t = type_name(p);
            expect(p, ")");
        }
        else
        {
            if (p->tok[-1].kind == TK_PUNCT && strcmp(p->tok[-1].text, "(") == 0)
                p->tok--;
            t = unary(p)->type;
        }
        /* size_t is unsigned long long on Windows x64 */
        return new_num(t->size, basic_type(TY_LLONG, 1), tok);
    }
    return postfix(p);
}

static struct node* cast_expr(struct parser* p)
{
    if (is(p, "("))
    {
        struct token* start = p->tok;
        p->tok++;
        if (is_typename(p))
        {
            struct type* t = type_name(p);
            expect(p, ")");
            struct node* operand = rv(cast_expr(p));
            struct node* n = new_node(N_CAST, start);
            n->lhs = operand;
            n->type = t;
            return n;
        }
        p->tok = start;
    }
    return unary(p);
}

static struct node* mul(struct parser* p)
{
    struct node* n = cast_expr(p);
    for (;;)
    {
        if (accept(p, "*")) n = new_arith(N_MUL, n, cast_expr(p));
        else if (accept(p, "/")) n = new_arith(N_DIV, n, cast_expr(p));
        else if (accept(p, "%")) n = new_arith(N_MOD, n, cast_expr(p));
        else return n;
    }
}

static struct node* add(struct parser* p)
{
    struct node* n = mul(p);
    for (;;)
    {
        struct token* tok = p->tok;
        if (accept(p, "+")) n = new_add(n, mul(p), tok);
        else if (accept(p, "-")) n = new_sub(n, mul(p), tok);
        else return n;
    }
}

static struct node* shift(struct parser* p)
{
    struct node* n = add(p);
    for (;;)
    {
        if (accept(p, "<<")) n = new_arith(N_SHL, n, add(p));
        else if (accept(p, ">>")) n = new_arith(N_SHR, n, add(p));
        else return n;
    }
}

static struct node* relational(struct parser* p)
{
    struct node* n = shift(p);
    for (;;)
    {
        if (accept(p, "<")) n = new_compare(N_LT, n, shift(p));
        else if (accept(p, "<=")) n = new_compare(N_LE, n, shift(p));
        else if (accept(p, ">")) n = new_compare(N_LT, shift(p), n);
        else if (accept(p, ">=")) n = new_compare(N_LE, shift(p), n);
        else return n;
    }
}

static struct node* equality(struct parser* p)
{
    struct node* n = relational(p);
    for (;;)
    {
        if (accept(p, "==")) n = new_compare(N_EQ, n, relational(p));
        else if (accept(p, "!=")) n = new_compare(N_NE, n, relational(p));
        else return n;
    }
}

static struct node* bitand(struct parser* p)
{
    struct node* n = equality(p);
    while (accept(p, "&"))
        n = new_arith(N_BITAND, n, equality(p));
    return n;
}

static struct node* bitxor(struct parser* p)
{
    struct node* n = bitand(p);
    while (accept(p, "^"))
        n = new_arith(N_BITXOR, n, bitand(p));
    return n;
}

static struct node* bitor(struct parser* p)
{
    struct node* n = bitxor(p);
    while (accept(p, "|"))
        n = new_arith(N_BITOR, n, bitxor(p));
    return n;
}

static struct node* logand(struct parser* p)
{
    struct node* n = bitor(p);
    while (accept(p, "&&"))
        n = new_binary(N_LOGAND, rv(n), rv(bitor(p)), basic_type(TY_INT, 0));
    return n;
}

static struct node* logor(struct parser* p)
{
    struct node* n = logand(p);
    while (accept(p, "||"))
        n = new_binary(N_LOGOR, rv(n), rv(logand(p)), basic_type(TY_INT, 0));
    return n;
}

static struct node* conditional(struct parser* p)
{
    struct node* c = logor(p);
    struct token* tok = p->tok;
    if (!accept(p, "?"))
        return c;

    struct node* n = new_node(N_COND, tok);
    n->cond = rv(c);
    struct node* a = rv(expr(p));
    expect(p, ":");
    struct node* b = rv(conditional(p));

    struct type* t;
    if (type_is_arith(a->type) && type_is_arith(b->type))
        t = usual_arith(a->type, b->type);
    else if (a->type->kind == TY_PTR && !is_null_const(a))
        t = (b->type->kind == TY_PTR && b->type->base->kind == TY_VOID) ? b->type : a->type;
    else
        t = b->type;

    n->then = conv(a, t);
    n->els = conv(b, t);
    n->type = t;
    return n;
}

static struct node* assign(struct parser* p)
{
    struct node* n = conditional(p);
    struct token* tok = p->tok;

    if (accept(p, "="))
    {
        struct node* rhs = rv(assign(p));
        return new_binary(N_ASSIGN, n, conv(rhs, n->type), n->type);
    }

    static const struct { const char* op; enum node_kind kind; } ops[] = {
        { "+=", N_ADD }, { "-=", N_SUB }, { "*=", N_MUL }, { "/=", N_DIV },
        { "%=", N_MOD }, { "<<=", N_SHL }, { ">>=", N_SHR }, { "&=", N_BITAND },
        { "|=", N_BITOR }, { "^=", N_BITXOR }
    };
    for (int i = 0; i < 10; i++)
        if (accept(p, ops[i].op))
            return new_assign_op(ops[i].kind, n, assign(p), 0, tok);
    return n;
}

static struct node* expr(struct parser* p)
{
    struct node* n = assign(p);
    while (accept(p, ","))
    {
        struct node* rhs = rv(assign(p));
        n = new_binary(N_COMMA, rv(n), rhs, rhs->type);
    }
    return n;
}

/* --------------------------------------------- constant evaluation */

/*
 * The input already has its constants folded (cake does it), so this
 * only understands the forms that remain: numbers, casts, unary minus,
 * simple arithmetic and addresses of static objects.
 */
static long long eval(struct node* n, const char** label)
{
    switch (n->kind)
    {
        case N_NUM: return (long long)n->ival;
        case N_FNUM: return (long long)n->fval;
        case N_NEG: return -eval(n->lhs, label);
        case N_BITNOT: return ~eval(n->lhs, label);
        case N_NOT: return !eval(n->lhs, label);
        case N_ADD: return eval(n->lhs, label) + eval(n->rhs, label);
        case N_SUB: return eval(n->lhs, label) - eval(n->rhs, label);
        case N_MUL: return eval(n->lhs, label) * eval(n->rhs, label);
        case N_DIV: return eval(n->lhs, label) / eval(n->rhs, label);
        case N_SHL: return eval(n->lhs, label) << eval(n->rhs, label);
        case N_SHR: return eval(n->lhs, label) >> eval(n->rhs, label);
        case N_BITAND: return eval(n->lhs, label) & eval(n->rhs, label);
        case N_BITOR: return eval(n->lhs, label) | eval(n->rhs, label);
        case N_COND: return eval(n->cond, label) ? eval(n->then, label) : eval(n->els, label);
        case N_CAST:
        {
            if (type_is_float(n->lhs->type) && type_is_integer(n->type))
            {
                struct node* f = n->lhs;
                while (f->kind == N_CAST) f = f->lhs;
                if (f->kind == N_FNUM) return (long long)f->fval;
            }
            long long v = eval(n->lhs, label);
            if (type_is_integer(n->type) && n->type->size < 8)
            {
                int bits = n->type->size * 8;
                unsigned long long mask = (1ull << bits) - 1;
                v = (long long)((unsigned long long)v & mask);
                if (!n->type->is_unsigned && (v >> (bits - 1)) & 1)
                    v = (long long)((unsigned long long)v | ~mask);
            }
            return v;
        }
        case N_ADDR:
        {
            struct node* x = n->lhs;
            long long offset = 0;
            for (;;)
            {
                if (x->kind == N_MEMBER) { offset += x->member->offset; x = x->lhs; continue; }
                if (x->kind == N_DEREF) return offset + eval(x->lhs, label);
                break;
            }
            if (x->kind != N_VAR || x->var->is_local)
                break;
            *label = x->var->label;
            return offset;
        }
        default:
            break;
    }
    cc_error_at(n->tok, "initializer is not a constant");
    return 0;
}

static double eval_double(struct node* n)
{
    while (n->kind == N_CAST && type_is_float(n->lhs->type)) n = n->lhs;
    if (n->kind == N_FNUM) return n->fval;
    if (n->kind == N_NEG) return -eval_double(n->lhs);
    if (n->kind == N_CAST && type_is_integer(n->lhs->type))
    {
        const char* label = NULL;
        long long v = eval(n->lhs, &label);
        return n->lhs->type->is_unsigned ? (double)(unsigned long long)v : (double)v;
    }
    const char* label = NULL;
    return (double)eval(n, &label);
}

static long long const_int(struct parser* p)
{
    const char* label = NULL;
    struct node* n = conditional(p);
    long long v = eval(n, &label);
    if (label)
        cc_error_at(n->tok, "expected an integer constant");
    return v;
}

/* --------------------------------------------------------- initializers */

/*
 * The initializer is flattened into a list of (offset, scalar type, expr).
 * Braces may be omitted (spec 5.3); a missing member stays zero.
 */
struct init_ctx
{
    struct parser* p;
    struct init_item head;
    struct init_item* tail;
    int max_end;              /* highest byte written, for unknown array size */
};

static void add_item(struct init_ctx* ic, int offset, struct type* t, struct member* bf, struct node* e)
{
    struct init_item* it = cc_alloc(sizeof *it);
    it->offset = offset;
    it->type = t;
    it->bitfield = bf;
    it->expr = e;
    ic->tail = ic->tail->next = it;
    if (offset + t->size > ic->max_end)
        ic->max_end = offset + t->size;
}

static void init_value(struct init_ctx* ic, struct type* t, int offset, struct member* bf);

static int at_list_end(struct parser* p)
{
    return is(p, "}") || (is(p, ",") && p->tok[1].kind == TK_PUNCT && strcmp(p->tok[1].text, "}") == 0);
}

static int string_init(struct init_ctx* ic, struct type* t, int offset)
{
    struct parser* p = ic->p;
    if (!(t->kind == TY_ARRAY && t->base->kind == TY_CHAR && p->tok->kind == TK_STRING))
        return 0;
    struct token* s = p->tok++;
    int n = t->array_len < 0 ? s->str_len + 1 : t->array_len;
    for (int i = 0; i < n && i <= s->str_len; i++)
        add_item(ic, offset + i, t->base, NULL, new_num((unsigned long long)(long long)(signed char)(i < s->str_len ? s->str[i] : 0), t->base, s));
    if (t->array_len < 0 && offset + n > ic->max_end)
        ic->max_end = offset + n;
    return 1;
}

/* elements of an aggregate without its own braces: consume what fits */
static void init_elided(struct init_ctx* ic, struct type* t, int offset)
{
    struct parser* p = ic->p;
    if (string_init(ic, t, offset))
        return;
    if (t->kind == TY_ARRAY)
    {
        for (int i = 0; i < t->array_len && !at_list_end(p); i++)
        {
            if (i > 0) expect(p, ",");
            init_value(ic, t->base, offset + i * t->base->size, NULL);
        }
        return;
    }
    if (type_is_record(t))
    {
        for (struct member* m = t->members; m && !at_list_end(p); m = m->next)
        {
            if (m->is_bitfield && m->name == NULL) continue;
            if (m != t->members) expect(p, ",");
            init_value(ic, m->type, offset + m->offset, m->is_bitfield ? m : NULL);
            if (t->kind == TY_UNION) break;
        }
        return;
    }
    add_item(ic, offset, t, NULL, conv(rv(assign(p)), t));
}

static void init_value(struct init_ctx* ic, struct type* t, int offset, struct member* bf)
{
    struct parser* p = ic->p;

    if (string_init(ic, t, offset))
        return;

    if (accept(p, "{"))
    {
        if (string_init(ic, t, offset))
        {
            accept(p, ",");
            expect(p, "}");
            return;
        }
        if (t->kind == TY_ARRAY)
        {
            int i = 0;
            while (!accept(p, "}"))
            {
                if (i > 0) expect(p, ",");
                if (accept(p, "}")) break;
                if (t->array_len >= 0 && i >= t->array_len)
                    cc_error_at(p->tok, "too many initializers");
                init_value(ic, t->base, offset + i * t->base->size, NULL);
                i++;
            }
            if (t->array_len < 0 && offset + i * t->base->size > ic->max_end)
                ic->max_end = offset + i * t->base->size;
            return;
        }
        if (type_is_record(t))
        {
            struct member* m = t->members;
            int first = 1;
            while (!accept(p, "}"))
            {
                if (!first) expect(p, ",");
                first = 0;
                if (accept(p, "}")) break;
                while (m && m->is_bitfield && m->name == NULL) m = m->next;
                if (m == NULL)
                    cc_error_at(p->tok, "too many initializers");
                init_value(ic, m->type, offset + m->offset, m->is_bitfield ? m : NULL);
                m = t->kind == TY_UNION ? NULL : m->next;
            }
            return;
        }
        /* scalar in braces */
        init_value(ic, t, offset, bf);
        accept(p, ",");
        expect(p, "}");
        return;
    }

    if (type_is_record(t) || t->kind == TY_ARRAY)
    {
        /* struct initialized by an expression of the same type? */
        if (type_is_record(t))
        {
            struct token* save = p->tok;
            struct node* e = assign(p);
            if (type_is_record(e->type))
            {
                add_item(ic, offset, t, NULL, e);
                return;
            }
            p->tok = save;
        }
        init_elided(ic, t, offset);
        return;
    }

    add_item(ic, offset, t, bf, conv(rv(assign(p)), t));
}

/* parses "= initializer"; may complete the array size of var->type */
static struct init_item* initializer(struct parser* p, struct obj* var)
{
    struct init_ctx ic = { 0 };
    ic.p = p;
    ic.tail = &ic.head;
    init_value(&ic, var->type, 0, NULL);

    if (var->type->kind == TY_ARRAY && var->type->array_len < 0)
    {
        int n = (ic.max_end + var->type->base->size - 1) / var->type->base->size;
        var->type = array_of(var->type->base, n);
    }
    return ic.head.next;
}

static void write_int(unsigned char* buf, int size, unsigned long long v)
{
    for (int i = 0; i < size; i++)
        buf[i] = (unsigned char)(v >> (8 * i));
}

/* turn the items of a static object into bytes and relocations */
static void build_static_data(struct obj* var, struct init_item* items)
{
    var->data = cc_alloc(var->type->size > 0 ? var->type->size : 1);
    for (struct init_item* it = items; it; it = it->next)
    {
        unsigned char* at = var->data + it->offset;

        if (type_is_record(it->type))
            cc_error_at(it->expr->tok, "initializer is not a constant");

        if (type_is_float(it->type))
        {
            double d = eval_double(it->expr);
            if (it->type->kind == TY_FLOAT)
            {
                float f = (float)d;
                memcpy(at, &f, 4);
            }
            else
                memcpy(at, &d, 8);
            continue;
        }

        const char* label = NULL;
        long long v = eval(it->expr, &label);
        if (label)
        {
            if (it->type->size != 8)
                cc_error_at(it->expr->tok, "address constant does not fit");
            struct reloc* r = cc_alloc(sizeof *r);
            r->offset = it->offset;
            r->label = label;
            r->addend = v;
            r->next = var->relocs;
            var->relocs = r;
            continue;
        }

        if (it->bitfield)
        {
            struct member* m = it->bitfield;
            unsigned long long unit = 0;
            for (int i = 0; i < m->type->size; i++)
                unit |= (unsigned long long)at[i] << (8 * i);
            unsigned long long mask = (m->bit_width == 64 ? ~0ull : ((1ull << m->bit_width) - 1)) << m->bit_offset;
            unit = (unit & ~mask) | (((unsigned long long)v << m->bit_offset) & mask);
            write_int(at, m->type->size, unit);
            continue;
        }
        write_int(at, it->type->size, (unsigned long long)v);
    }
}

/* ------------------------------------------------------------ pragmas */

static void pragma(struct parser* p)
{
    struct token* tok = p->tok++;
    const char* s = tok->text;
    while (*s == ' ' || *s == '\t') s++;

    if (strncmp(s, "pack", 4) != 0)
    {
        cc_warning(tok->file, tok->line, tok->col, "unknown pragma ignored");
        return;
    }
    int n = 0;
    if (strstr(s, "push"))
    {
        if (p->pack_depth < 32)
            p->pack_stack[p->pack_depth++] = p->pack;
        const char* comma = strchr(s, ',');
        if (comma)
            p->pack = atoi(comma + 1);
    }
    else if (strstr(s, "pop"))
    {
        p->pack = p->pack_depth > 0 ? p->pack_stack[--p->pack_depth] : 0;
    }
    else if (sscanf(s, "pack ( %d", &n) == 1 || sscanf(s, "pack(%d", &n) == 1)
        p->pack = n;
    else
        p->pack = 0;
}

/* --------------------------------------------------------- statements */

static struct stmt* new_stmt(enum stmt_kind kind)
{
    struct stmt* s = cc_alloc(sizeof *s);
    s->kind = kind;
    return s;
}

static struct node* condition(struct parser* p)
{
    expect(p, "(");
    struct node* n = rv(expr(p));
    expect(p, ")");
    return n;
}

static struct stmt* stmt(struct parser* p);

/* declarations inside a block; returns a list of S_INIT */
static struct stmt* local_declaration(struct parser* p)
{
    struct stmt head = { 0 };
    struct stmt* cur = &head;
    enum storage sc;
    struct type* base = declspec(p, &sc);

    int first = 1;
    while (!accept(p, ";"))
    {
        if (!first)
            expect(p, ",");
        first = 0;

        struct token* name = NULL;
        struct type* t = declarator(p, base, &name);
        if (name == NULL)
            cc_error_at(p->tok, "expected a name in declaration");

        if (sc == SC_EXTERN || t->kind == TY_FUNC)
        {
            /* refers to the object or function of file scope */
            struct obj* var = find_var_in(file_scope(p), name->text);
            if (var == NULL)
            {
                var = new_obj(name->text, t);
                var->is_function = t->kind == TY_FUNC;
                push_var(file_scope(p), name->text, var);
                add_global(p, var);
            }
            push_var(p->scope, name->text, var);
            continue;
        }

        if (sc == SC_STATIC)
        {
            struct obj* var = new_anon_static(p, name->text, t);
            var->tok = name;
            push_var(p->scope, name->text, var);
            if (accept(p, "="))
                build_static_data(var, initializer(p, var));
            continue;
        }

        struct obj* var = new_local(p, name->text, t);
        var->tok = name;
        if (accept(p, "="))
        {
            struct stmt* s = new_stmt(S_INIT);
            s->tok = name;
            s->var = var;
            s->items = initializer(p, var);
            cur = cur->next = s;
        }
    }
    return head.next;
}

static struct stmt* compound_stmt(struct parser* p)
{
    /* the '{' was consumed */
    struct stmt* block = new_stmt(S_BLOCK);
    struct stmt head = { 0 };
    struct stmt* cur = &head;

    enter_scope(p);
    while (!accept(p, "}"))
    {
        if (p->tok->kind == TK_EOF)
            cc_error_at(p->tok, "expected '}'");
        if (p->tok->kind == TK_PRAGMA)
        {
            pragma(p);
            continue;
        }
        struct stmt* s = is_typename(p) ? local_declaration(p) : stmt(p);
        cur->next = s;
        while (cur->next) cur = cur->next;
    }
    leave_scope(p);
    block->body = head.next;
    return block;
}

static struct stmt* stmt_kind(struct parser* p);

/* every statement remembers its first token (file and line) */
static struct stmt* stmt(struct parser* p)
{
    struct token* tok = p->tok;
    struct stmt* s = stmt_kind(p);
    if (s->tok == NULL)
        s->tok = tok;
    return s;
}

static struct stmt* stmt_kind(struct parser* p)
{
    struct token* tok = p->tok;

    if (accept(p, "{"))
        return compound_stmt(p);

    if (accept(p, "if"))
    {
        struct stmt* s = new_stmt(S_IF);
        s->expr = condition(p);
        s->then = stmt(p);
        if (accept(p, "else"))
            s->els = stmt(p);
        return s;
    }
    if (accept(p, "while"))
    {
        struct stmt* s = new_stmt(S_WHILE);
        s->expr = condition(p);
        s->body = stmt(p);
        return s;
    }
    if (accept(p, "do"))
    {
        struct stmt* s = new_stmt(S_DO);
        s->body = stmt(p);
        expect(p, "while");
        s->expr = condition(p);
        expect(p, ";");
        return s;
    }
    if (accept(p, "for"))
    {
        struct stmt* s = new_stmt(S_FOR);
        expect(p, "(");
        if (!is(p, ";")) s->init = expr(p);
        expect(p, ";");
        if (!is(p, ";")) s->expr = rv(expr(p));
        expect(p, ";");
        if (!is(p, ")")) s->inc = expr(p);
        expect(p, ")");
        s->body = stmt(p);
        return s;
    }
    if (accept(p, "goto"))
    {
        struct stmt* s = new_stmt(S_GOTO);
        s->label = expect_ident(p)->text;
        expect(p, ";");
        return s;
    }
    if (accept(p, "break"))
    {
        expect(p, ";");
        return new_stmt(S_BREAK);
    }
    if (accept(p, "continue"))
    {
        expect(p, ";");
        return new_stmt(S_CONTINUE);
    }
    if (accept(p, "return"))
    {
        struct stmt* s = new_stmt(S_RETURN);
        if (!accept(p, ";"))
        {
            struct node* e = rv(expr(p));
            struct type* ret = p->fn->type->base;
            s->expr = ret->kind == TY_VOID ? e : conv(e, ret);
            expect(p, ";");
        }
        return s;
    }
    if (is(p, "switch") || is(p, "case") || is(p, "default"))
        cc_error_at(tok, "switch is not supported (cake output does not use it)");

    if (tok->kind == TK_IDENT && tok[1].kind == TK_PUNCT && strcmp(tok[1].text, ":") == 0)
    {
        p->tok += 2;
        struct stmt* s = new_stmt(S_LABEL);
        s->label = tok->text;
        s->body = is(p, "}") ? new_stmt(S_BLOCK) : stmt(p);
        return s;
    }

    struct stmt* s = new_stmt(S_EXPR);
    if (!is(p, ";"))
        s->expr = expr(p);
    expect(p, ";");
    return s;
}

/* ------------------------------------------------- external definitions */

static struct obj* declare_global(struct parser* p, struct token* name, struct type* t, enum storage sc)
{
    struct scope* fs = file_scope(p);
    struct obj* var = find_var_in(fs, name->text);
    if (var)
    {
        /* redeclaration: keep the most complete type */
        if (t->kind == TY_ARRAY && t->array_len >= 0)
            var->type = t;
        if (t->kind == TY_FUNC && t->has_prototype && !var->type->has_prototype)
            var->type = t;
        return var;
    }
    var = new_obj(name->text, t);
    var->tok = name;
    var->is_function = t->kind == TY_FUNC;
    if (sc == SC_STATIC)
    {
        /* internal linkage: the label must not clash with other units */
        char buf[256];
        snprintf(buf, sizeof buf, "%s.%d", name->text, p->unit);
        var->label = cc_strndup(buf, (int)strlen(buf));
        var->is_static = 1;
    }
    push_var(fs, name->text, var);
    add_global(p, var);
    return var;
}

static void function_definition(struct parser* p, struct obj* fn)
{
    struct type* ft = fn->type;
    fn->is_defined = 1;
    p->fn = fn;
    p->locals_tail = NULL;
    fn->locals = NULL;

    enter_scope(p);
    struct obj ph = { 0 };
    struct obj* cur = &ph;
    int index = 0;
    for (struct param* pa = ft->params; pa; pa = pa->next)
    {
        struct obj* var = new_local(p, pa->name ? pa->name : "", pa->type);
        var->tok = pa->tok;
        var->param_index = index++;
        cur = cur->next = var;
    }
    fn->params = ph.next;

    expect(p, "{");
    fn->body = compound_stmt(p);
    leave_scope(p);
    p->fn = NULL;
}

static void external_declaration(struct parser* p)
{
    enum storage sc;
    p->selectany = 0;
    struct type* base = declspec(p, &sc);
    int selectany = p->selectany;

    int first = 1;
    while (!accept(p, ";"))
    {
        if (!first)
            expect(p, ",");

        struct token* name = NULL;
        struct type* t = declarator(p, base, &name);
        if (name == NULL)
            cc_error_at(p->tok, "expected a name in declaration");

        struct obj* var = declare_global(p, name, t, sc);
        if (selectany)
            var->is_selectany = 1;

        if (t->kind == TY_FUNC)
        {
            if (first && is(p, "{"))
            {
                /* the parameter names come from this declarator */
                var->type = t;
                var->tok = name;
                function_definition(p, var);
                return;
            }
            first = 0;
            continue;
        }
        first = 0;

        if (accept(p, "="))
        {
            struct init_item* items = initializer(p, var);
            var->type = var->type->size ? var->type : t;
            build_static_data(var, items);
            var->is_defined = 1;
            var->is_tentative = 0;
        }
        else if (sc != SC_EXTERN && !var->is_defined)
        {
            var->is_defined = 1;
            var->is_tentative = 1;
        }
    }
}

void parse_program(struct token_list* tokens, struct program* out, int unit, const struct target* target)
{
    struct parser p = { 0 };
    p.unit = unit;
    p.target = target;
    out->target = target;
    for (p.globals_tail = out->globals; p.globals_tail && p.globals_tail->next;)
        p.globals_tail = p.globals_tail->next;
    p.tok = tokens->data;
    p.prog = out;
    enter_scope(&p);

    while (p.tok->kind != TK_EOF)
    {
        if (p.tok->kind == TK_PRAGMA)
        {
            pragma(&p);
            continue;
        }
        if (accept(&p, ";"))
            continue;
        external_declaration(&p);
    }
}
