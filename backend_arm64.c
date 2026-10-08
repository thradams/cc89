/*
 * Back-end: AArch64 (arm64). Writes assembly text that asm_arm64.c
 * turns into machine code. Same stack machine as backend_x64.c:
 *
 *   - every expression leaves its value in one place:
 *       integers and pointers  -> x0 (always extended to 64 bits
 *                                 according to the type)
 *       float, double          -> s0, d0
 *       struct, union          -> x0 holds the ADDRESS of the value
 *   - a binary operator evaluates the left side, pushes it, evaluates
 *     the right side and pops the left side back (into x1/d1).
 *     sp must stay a multiple of 16, so every push takes 16 bytes:
 *         str x0, [sp, #-16]!        push
 *         ldr x1, [sp], #16          pop
 *   - every local lives in memory, at a negative offset from x29.
 *
 * Registers: x0, x1 values; x9-x12 temporaries; x16, x17 for long
 * immediates and calls through pointers; x29 frame pointer; x30 return
 * address. Only registers a call may destroy are used, so the prologue
 * saves nothing but x29 and x30:
 *
 *        stp x29, x30, [sp, #-16]!    save frame pointer and return address
 *        mov x29, sp
 *        sub sp, sp, #frame           locals and temporaries
 *
 *        [x29+16]  arguments on the stack
 *        [x29+8]   return address (x30)
 *        [x29]     caller's x29
 *        [x29-n]   locals and temporaries
 *
 * Calling convention (AAPCS64):
 *   - integers and pointers in x0..x7, float/double in v0..v7 (s/d)
 *   - a struct of up to 16 bytes travels in 1 or 2 x registers; a struct
 *     of 1 to 4 members of the same float type (an "HFA") travels in
 *     v registers, one member each; bigger structs are passed as a
 *     pointer to a copy
 *   - a struct result bigger than 16 bytes (not an HFA) is written to
 *     the memory whose address the caller passes in x8
 *   - what does not fit in registers goes on the stack
 *   - Apple (macOS): the variadic arguments ("...") always go on the
 *     stack, 8 bytes each; va_list is a char*. Linux: they use the
 *     registers like the others, and va_list is a struct (see va_arg).
 */
#include "backend.h"
#include "dwarf.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

enum arg_kind { K_INT, K_FP, K_HFA, K_INDIRECT };

struct arg_class
{
    enum arg_kind kind;
    int n;          /* registers used: x registers (K_INT), v registers (K_FP, K_HFA) */
    int fsize;      /* K_FP, K_HFA: 4 (float) or 8 (double) */
};

struct cg
{
    struct text* out;
    struct text body;
    struct text prologue;
    struct obj* fn;
    int apple;              /* macOS ABI */
    struct dwarf* dw;       /* -g */
    int depth;              /* 16-byte slots pushed on the stack */
    int frame_size;
    int label_count;
    int break_label;
    int continue_label;
    int return_label;

    int indirect_ret_offset;    /* where x8 was saved, for big struct results */
    int named_gr, named_vr;     /* registers used by the named parameters */
    int named_stack;            /* bytes of named parameters on the stack */
    int gr_save_offset;         /* Linux variadic: x0-x7 saved here (64 bytes) */
    int vr_save_offset;         /* Linux variadic: q0-q7 saved here (128 bytes) */
};

static void gen_expr(struct cg* cg, struct node* n);
static void gen_addr(struct cg* cg, struct node* n);
static void gen_stmt(struct cg* cg, struct stmt* s);

static void emit(struct cg* cg, const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    text_append(&cg->body, buf, n);
    text_append(&cg->body, "\n", 1);
}

static int new_label(struct cg* cg)
{
    return cg->label_count++;
}

static int align_to(int n, int align)
{
    return (n + align - 1) / align * align;
}

static int alloc_temp(struct cg* cg, int size, int align)
{
    if (align < 8) align = 8;
    cg->frame_size = align_to(cg->frame_size + size, align);
    return -cg->frame_size;
}

static int is_flt(struct type* t)
{
    if (t->kind == TY_LDOUBLE)
        cc_error(NULL, 0, 0, "long double is not supported on arm64");
    return type_is_float(t);
}

/* register name of the value: "d0" or "s0" */
static const char* fpreg(struct type* t, int n)
{
    static const char* d[] = { "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7" };
    static const char* s[] = { "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7" };
    return t->kind == TY_FLOAT ? s[n] : d[n];
}

/* ------------------------------------------------------------ constants */

/* load any 64-bit value: movz the first 16 bits, movk the others */
static void load_imm(struct cg* cg, const char* reg, unsigned long long v)
{
    emit(cg, "    movz %s, #%llu", reg, v & 0xFFFF);
    for (int shift = 16; shift < 64; shift += 16)
        if ((v >> shift) & 0xFFFF)
            emit(cg, "    movk %s, #%llu, lsl #%d", reg, (v >> shift) & 0xFFFF, shift);
}

/* reg = base + offset (offset may be negative or large) */
static void add_offset(struct cg* cg, const char* reg, const char* base, long long offset)
{
    const char* op = offset < 0 ? "sub" : "add";
    unsigned long long v = (unsigned long long)(offset < 0 ? -offset : offset);
    if (v < 4096)
        emit(cg, "    %s %s, %s, #%llu", op, reg, base, v);
    else if (v < (1ull << 24))
    {
        emit(cg, "    %s %s, %s, #%llu, lsl #12", op, reg, base, v >> 12);
        if (v & 0xFFF)
            emit(cg, "    %s %s, %s, #%llu", op, reg, reg, v & 0xFFF);
    }
    else
    {
        load_imm(cg, "x16", v);
        emit(cg, "    %s %s, %s, x16", op, reg, base);
    }
}

/* ---------------------------------------------------------------- stack */

static void push_x(struct cg* cg, const char* reg)
{
    emit(cg, "    str %s, [sp, #-16]!", reg);
    cg->depth++;
}

static void pop_x(struct cg* cg, const char* reg)
{
    emit(cg, "    ldr %s, [sp], #16", reg);
    cg->depth--;
}

static void push_value(struct cg* cg, struct type* t)
{
    if (is_flt(t)) push_x(cg, "d0");
    else push_x(cg, "x0");
}

/* ------------------------------------------------------- load / store */

/* re-extend x0 to 64 bits as the type says */
static void normalize(struct cg* cg, struct type* t)
{
    if (!type_is_integer(t))
        return;
    switch (t->size)
    {
        case 1: emit(cg, t->is_unsigned ? "    uxtb w0, w0" : "    sxtb x0, w0"); break;
        case 2: emit(cg, t->is_unsigned ? "    uxth w0, w0" : "    sxth x0, w0"); break;
        case 4: emit(cg, t->is_unsigned ? "    mov w0, w0" : "    sxtw x0, w0"); break;
    }
}

/* x0 has an address; replace it by the value stored there */
static void load(struct cg* cg, struct type* t)
{
    if (t->kind == TY_ARRAY || t->kind == TY_FUNC || type_is_record(t))
        return;
    if (is_flt(t))
    {
        emit(cg, t->kind == TY_FLOAT ? "    ldr s0, [x0]" : "    ldr d0, [x0]");
        return;
    }
    switch (t->size)
    {
        case 1: emit(cg, t->is_unsigned ? "    ldrb w0, [x0]" : "    ldrsb x0, [x0]"); break;
        case 2: emit(cg, t->is_unsigned ? "    ldrh w0, [x0]" : "    ldrsh x0, [x0]"); break;
        case 4: emit(cg, t->is_unsigned ? "    ldr w0, [x0]" : "    ldrsw x0, [x0]"); break;
        default: emit(cg, "    ldr x0, [x0]"); break;
    }
}

/* copy size bytes from [x0] to [x1]; x0 and x1 are kept */
static void copy_bytes(struct cg* cg, int size)
{
    if (size > 256)
    {
        /* a loop of 8-byte moves, then the rest */
        int loop = new_label(cg);
        emit(cg, "    mov x9, x0");
        emit(cg, "    mov x10, x1");
        load_imm(cg, "x11", (unsigned long long)(size / 8));
        emit(cg, ".L%d:", loop);
        emit(cg, "    ldr x12, [x9], #8");
        emit(cg, "    str x12, [x10], #8");
        emit(cg, "    subs x11, x11, #1");
        emit(cg, "    b.ne .L%d", loop);
        for (int i = size / 8 * 8; i < size; i++)
        {
            emit(cg, "    ldrb w12, [x9], #1");
            emit(cg, "    strb w12, [x10], #1");
        }
        return;
    }
    int i = 0;
    for (; i + 8 <= size; i += 8)
    {
        emit(cg, "    ldr x9, [x0, #%d]", i);
        emit(cg, "    str x9, [x1, #%d]", i);
    }
    for (; i + 4 <= size; i += 4)
    {
        emit(cg, "    ldr w9, [x0, #%d]", i);
        emit(cg, "    str w9, [x1, #%d]", i);
    }
    for (; i < size; i++)
    {
        emit(cg, "    ldrb w9, [x0, #%d]", i);
        emit(cg, "    strb w9, [x1, #%d]", i);
    }
}

/* store the value (x0 / s0 / d0) at the address in x1 */
static void store(struct cg* cg, struct type* t)
{
    if (type_is_record(t))
    {
        copy_bytes(cg, t->size);
        emit(cg, "    mov x0, x1");
        return;
    }
    if (is_flt(t))
    {
        emit(cg, t->kind == TY_FLOAT ? "    str s0, [x1]" : "    str d0, [x1]");
        return;
    }
    switch (t->size)
    {
        case 1: emit(cg, "    strb w0, [x1]"); break;
        case 2: emit(cg, "    strh w0, [x1]"); break;
        case 4: emit(cg, "    str w0, [x1]"); break;
        default: emit(cg, "    str x0, [x1]"); break;
    }
}

/*
 * Bit-fields: ubfx/sbfx extract a field (sbfx extends the sign), bfi
 * inserts one; the rest of the storage unit is kept.
 */
static void load_bitfield(struct cg* cg, struct member* m)
{
    struct type unit = *m->type;
    unit.is_unsigned = 1;
    load(cg, &unit);
    emit(cg, "    %s x0, x0, #%d, #%d", m->type->is_unsigned ? "ubfx" : "sbfx", m->bit_offset, m->bit_width);
}

/* value in x0, address of the unit in x1; x0 is kept */
static void store_bitfield(struct cg* cg, struct member* m)
{
    switch (m->type->size)
    {
        case 1: emit(cg, "    ldrb w9, [x1]"); break;
        case 2: emit(cg, "    ldrh w9, [x1]"); break;
        case 4: emit(cg, "    ldr w9, [x1]"); break;
        default: emit(cg, "    ldr x9, [x1]"); break;
    }
    emit(cg, "    bfi x9, x0, #%d, #%d", m->bit_offset, m->bit_width);
    switch (m->type->size)
    {
        case 1: emit(cg, "    strb w9, [x1]"); break;
        case 2: emit(cg, "    strh w9, [x1]"); break;
        case 4: emit(cg, "    str w9, [x1]"); break;
        default: emit(cg, "    str x9, [x1]"); break;
    }
}

static struct member* bitfield_of(struct node* n)
{
    return n->kind == N_MEMBER && n->member->is_bitfield ? n->member : NULL;
}

static void load_lvalue(struct cg* cg, struct node* lv)
{
    struct member* bf = bitfield_of(lv);
    if (bf) load_bitfield(cg, bf);
    else load(cg, lv->type);
}

static void store_lvalue(struct cg* cg, struct node* lv)
{
    struct member* bf = bitfield_of(lv);
    if (bf) store_bitfield(cg, bf);
    else store(cg, lv->type);
}

/* ------------------------------------------------------- conversions */

static void cast(struct cg* cg, struct type* from, struct type* to)
{
    if (to->kind == TY_VOID || type_is_record(to) || type_is_record(from))
        return;

    if (is_flt(from) && is_flt(to))
    {
        if (from->kind == TY_FLOAT && to->kind != TY_FLOAT) emit(cg, "    fcvt d0, s0");
        if (from->kind != TY_FLOAT && to->kind == TY_FLOAT) emit(cg, "    fcvt s0, d0");
        return;
    }
    if (is_flt(to))
    {
        /* x0 holds the exact 64-bit value: ucvtf for unsigned 64-bit */
        int u = from->is_unsigned && from->size == 8;
        emit(cg, "    %s %s, x0", u ? "ucvtf" : "scvtf", fpreg(to, 0));
        return;
    }
    if (is_flt(from))
    {
        /* truncating toward zero */
        int u = to->is_unsigned && to->size == 8;
        emit(cg, "    %s x0, %s", u ? "fcvtzu" : "fcvtzs", fpreg(from, 0));
        normalize(cg, to);
        return;
    }
    if (to->size < 8)
        normalize(cg, to);
}

/* jump to label when the value is zero (or not zero) */
static void branch_zero(struct cg* cg, struct type* t, int if_zero, int label)
{
    if (is_flt(t))
    {
        emit(cg, "    fcmp %s, #0.0", fpreg(t, 0));
        emit(cg, "    b.%s .L%d", if_zero ? "eq" : "ne", label);
    }
    else
        emit(cg, "    %s x0, .L%d", if_zero ? "cbz" : "cbnz", label);
}

/* ------------------------------------------------------- classification */

/* HFA: 1 to 4 members (also inside arrays and nested structs) of one float type */
static int hfa_members(struct type* t, int* fsize)
{
    if (type_is_float(t))
    {
        if (*fsize && *fsize != t->size) return -1;
        *fsize = t->size;
        return 1;
    }
    if (t->kind == TY_ARRAY)
    {
        int k = hfa_members(t->base, fsize);
        return k < 0 || t->array_len < 0 ? -1 : k * t->array_len;
    }
    if (t->kind == TY_STRUCT)
    {
        int total = 0;
        for (struct member* m = t->members; m; m = m->next)
        {
            if (m->is_bitfield) return -1;
            int k = hfa_members(m->type, fsize);
            if (k < 0) return -1;
            total += k;
        }
        return total;
    }
    if (t->kind == TY_UNION)
    {
        int max = 0;
        for (struct member* m = t->members; m; m = m->next)
        {
            int k = hfa_members(m->type, fsize);
            if (k < 0) return -1;
            if (k > max) max = k;
        }
        return max ? (max * *fsize == t->size ? max : -1) : -1;
    }
    return -1;
}

static struct arg_class classify(struct type* t)
{
    struct arg_class c = { K_INT, 1, 0 };
    if (is_flt(t))
    {
        c.kind = K_FP;
        c.fsize = t->size;
        return c;
    }
    if (!type_is_record(t))
        return c;
    int fsize = 0;
    int k = hfa_members(t, &fsize);
    if (k >= 1 && k <= 4 && k * fsize == t->size)
    {
        c.kind = K_HFA;
        c.n = k;
        c.fsize = fsize;
        return c;
    }
    if (t->size > 16)
    {
        c.kind = K_INDIRECT;
        return c;
    }
    c.n = (t->size + 7) / 8;
    return c;
}

/* where one argument goes */
struct placement
{
    struct arg_class c;
    int on_stack;
    int stack_offset;
    int reg;            /* first x or v register */
};

/*
 * Assign registers and stack offsets, in order, as the callee will.
 * named: arguments before "..." (all of them without a prototype).
 */
static void place(struct cg* cg, struct type* t, int named, int* ngrn, int* nsrn, int* nsaa, struct placement* p)
{
    p->c = classify(t);
    p->on_stack = 0;

    if (cg->apple && !named)
    {
        /* Apple: variadic arguments on the stack, 8-byte slots */
        p->on_stack = 1;
        p->stack_offset = *nsaa = align_to(*nsaa, 8);
        int size = p->c.kind == K_INDIRECT ? 8 : t->size;
        *nsaa += align_to(size, 8);
        return;
    }

    int fp = p->c.kind == K_FP || p->c.kind == K_HFA;
    if (fp && *nsrn + p->c.n <= 8)
    {
        p->reg = *nsrn;
        *nsrn += p->c.n;
        return;
    }
    if (!fp && *ngrn + p->c.n <= 8)
    {
        p->reg = *ngrn;
        *ngrn += p->c.n;
        return;
    }
    if (fp) *nsrn = 8;
    else *ngrn = 8;

    /* on the stack: Linux uses 8-byte slots; Apple packs named
       arguments at their natural alignment */
    int size = p->c.kind == K_INDIRECT ? 8 : t->size;
    int align = p->c.kind == K_INDIRECT ? 8 : t->align;
    if (!cg->apple)
    {
        if (align < 8) align = 8;
        size = align_to(size, 8);
    }
    p->on_stack = 1;
    p->stack_offset = *nsaa = align_to(*nsaa, align);
    *nsaa += size;
}

/* ------------------------------------------------------- expressions */

static int param_indirect(struct obj* v)
{
    return v->param_index >= 0 && classify(v->type).kind == K_INDIRECT;
}

static void gen_addr(struct cg* cg, struct node* n)
{
    switch (n->kind)
    {
        case N_VAR:
        {
            struct obj* v = n->var;
            if (v->is_local)
            {
                add_offset(cg, "x0", "x29", v->offset);
                if (param_indirect(v))
                    emit(cg, "    ldr x0, [x0]");    /* the slot holds a pointer to a copy */
            }
            else if (!v->is_defined && !v->is_function)
            {
                /* object of a shared library: its address is in the GOT */
                emit(cg, "    adrp x0, :got:%s", v->label);
                emit(cg, "    ldr x0, [x0, :got_lo12:%s]", v->label);
            }
            else
            {
                emit(cg, "    adrp x0, %s", v->label);
                emit(cg, "    add x0, x0, :lo12:%s", v->label);
            }
            return;
        }
        case N_DEREF:
            gen_expr(cg, n->lhs);
            return;
        case N_MEMBER:
            gen_addr(cg, n->lhs);
            if (n->member->offset)
                add_offset(cg, "x0", "x0", n->member->offset);
            return;
        default:
            if (type_is_record(n->type))
            {
                gen_expr(cg, n);
                return;
            }
            break;
    }
    cc_error_at(n->tok, "not an lvalue");
}

/* left in x0/d0, right in x1/d1 */
static void binary_op(struct cg* cg, enum node_kind op, struct type* t)
{
    if (is_flt(t))
    {
        const char* r0 = fpreg(t, 0);
        const char* r1 = fpreg(t, 1);
        const char* name = op == N_ADD ? "fadd" : op == N_SUB ? "fsub" : op == N_MUL ? "fmul" : "fdiv";
        emit(cg, "    %s %s, %s, %s", name, r0, r0, r1);
        return;
    }
    /* integers: 64-bit operation on extended values, then re-extend */
    switch (op)
    {
        case N_ADD: emit(cg, "    add x0, x0, x1"); break;
        case N_SUB: emit(cg, "    sub x0, x0, x1"); break;
        case N_MUL: emit(cg, "    mul x0, x0, x1"); break;
        case N_BITAND: emit(cg, "    and x0, x0, x1"); break;
        case N_BITOR: emit(cg, "    orr x0, x0, x1"); break;
        case N_BITXOR: emit(cg, "    eor x0, x0, x1"); break;
        case N_SHL: emit(cg, "    lsl x0, x0, x1"); break;
        case N_SHR: emit(cg, "    %s x0, x0, x1", t->is_unsigned ? "lsr" : "asr"); break;
        case N_DIV: emit(cg, "    %s x0, x0, x1", t->is_unsigned ? "udiv" : "sdiv"); break;
        case N_MOD:
            /* remainder: x0 - (x0 / x1) * x1 */
            emit(cg, "    %s x2, x0, x1", t->is_unsigned ? "udiv" : "sdiv");
            emit(cg, "    msub x0, x2, x1, x0");
            break;
        default: break;
    }
    normalize(cg, t);
}

static void compare_op(struct cg* cg, enum node_kind op, struct type* t)
{
    if (is_flt(t))
    {
        /* fcmp: "less" is mi; unordered (NaN) makes lt, le and eq false */
        emit(cg, "    fcmp %s, %s", fpreg(t, 0), fpreg(t, 1));
        emit(cg, "    cset x0, %s", op == N_LT ? "mi" : op == N_LE ? "ls" : op == N_EQ ? "eq" : "ne");
        return;
    }
    int u = t->is_unsigned || t->kind == TY_PTR;
    emit(cg, "    cmp x0, x1");
    switch (op)
    {
        case N_EQ: emit(cg, "    cset x0, eq"); break;
        case N_NE: emit(cg, "    cset x0, ne"); break;
        case N_LT: emit(cg, "    cset x0, %s", u ? "lo" : "lt"); break;
        default:   emit(cg, "    cset x0, %s", u ? "ls" : "le"); break;
    }
}

static void gen_operands(struct cg* cg, struct node* lhs, struct node* rhs)
{
    gen_expr(cg, lhs);
    push_value(cg, lhs->type);
    gen_expr(cg, rhs);
    if (is_flt(rhs->type)) emit(cg, "    fmov %s, %s", fpreg(rhs->type, 1), fpreg(rhs->type, 0));
    else emit(cg, "    mov x1, x0");
    if (is_flt(lhs->type)) pop_x(cg, "d0");
    else pop_x(cg, "x0");
}

/* copy the struct at [x0] to a new 16-byte (or bigger) temporary; x0 = its address */
static int struct_to_temp(struct cg* cg, struct type* t)
{
    int tmp = alloc_temp(cg, align_to(t->size, 16) < 16 ? 16 : align_to(t->size, 16), 16);
    add_offset(cg, "x1", "x29", tmp);
    copy_bytes(cg, t->size);
    emit(cg, "    mov x0, x1");
    return tmp;
}

static void gen_call(struct cg* cg, struct node* n)
{
    struct type* ret = n->type;
    struct node* fn = n->lhs;
    struct type* ft = fn->type->base;
    int nparams = 0;
    for (struct param* p = ft->params; p; p = p->next)
        nparams++;

    struct arg_class rc = classify(ret);
    int ret_tmp = 0;
    if (type_is_record(ret))
        ret_tmp = alloc_temp(cg, align_to(ret->size, 16) < 16 ? 16 : align_to(ret->size, 16), 16);

    /* 1. where each argument goes */
    struct placement* pl = cc_alloc(sizeof(struct placement) * (n->nargs + 1));
    int ngrn = 0, nsrn = 0, nsaa = 0;
    for (int i = 0; i < n->nargs; i++)
    {
        int named = !ft->has_prototype || i < nparams;
        place(cg, n->args[i]->type, named, &ngrn, &nsrn, &nsaa, &pl[i]);
    }
    int stack_size = align_to(nsaa, 16);

    /* 2. stack arguments: reserve the area, evaluate each argument and
          store it at its offset (the pushes of an evaluation are undone
          before the store, so sp is the start of the area again) */
    if (stack_size)
    {
        add_offset(cg, "sp", "sp", -stack_size);
        cg->depth += stack_size / 16;
    }
    for (int i = 0; i < n->nargs; i++)
    {
        if (!pl[i].on_stack)
            continue;
        struct node* arg = n->args[i];
        gen_expr(cg, arg);
        int off = pl[i].stack_offset;
        if (pl[i].c.kind == K_INDIRECT)
        {
            struct_to_temp(cg, arg->type);
            add_offset(cg, "x1", "sp", off);
            emit(cg, "    str x0, [x1]");
        }
        else if (type_is_record(arg->type))
        {
            add_offset(cg, "x1", "sp", off);
            copy_bytes(cg, arg->type->size);
        }
        else
        {
            add_offset(cg, "x1", "sp", off);
            struct type* t = arg->type;
            if (is_flt(t)) emit(cg, "    str %s, [x1]", fpreg(t, 0));
            else if (cg->apple && t->size < 8)
                emit(cg, t->size == 1 ? "    strb w0, [x1]" : t->size == 2 ? "    strh w0, [x1]" : "    str w0, [x1]");
            else emit(cg, "    str x0, [x1]");
        }
    }

    /* 3. the function pointer */
    int direct = fn->kind == N_ADDR && fn->lhs->kind == N_VAR && fn->lhs->var->is_function;
    if (!direct)
    {
        gen_expr(cg, fn);
        push_x(cg, "x0");
    }

    /* 4. register arguments: every piece pushed, last first ... */
    for (int i = n->nargs - 1; i >= 0; i--)
    {
        if (pl[i].on_stack)
            continue;
        struct node* arg = n->args[i];
        gen_expr(cg, arg);
        struct arg_class c = pl[i].c;
        if (c.kind == K_INDIRECT)
        {
            struct_to_temp(cg, arg->type);
            push_x(cg, "x0");
        }
        else if (type_is_record(arg->type))
        {
            struct_to_temp(cg, arg->type);
            int step = c.kind == K_HFA ? c.fsize : 8;
            for (int j = c.n - 1; j >= 0; j--)
            {
                emit(cg, step == 4 ? "    ldr w9, [x0, #%d]" : "    ldr x9, [x0, #%d]", j * step);
                push_x(cg, "x9");
            }
        }
        else
            push_value(cg, arg->type);
    }

    /* ... then popped into x0..x7 / v0..v7, first first */
    static const char* xr[] = { "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7" };
    static const char* dr[] = { "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7" };
    static const char* sr[] = { "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7" };
    for (int i = 0; i < n->nargs; i++)
    {
        if (pl[i].on_stack)
            continue;
        struct arg_class c = pl[i].c;
        for (int j = 0; j < c.n; j++)
        {
            if (c.kind == K_FP || c.kind == K_HFA)
                pop_x(cg, c.fsize == 4 ? sr[pl[i].reg + j] : dr[pl[i].reg + j]);
            else
                pop_x(cg, xr[pl[i].reg + j]);
        }
    }

    if (type_is_record(ret) && rc.kind == K_INDIRECT)
        add_offset(cg, "x8", "x29", ret_tmp);    /* where the callee writes the result */
    if (!direct)
        pop_x(cg, "x16");

    if (direct)
        emit(cg, "    bl %s", fn->lhs->var->label);
    else
        emit(cg, "    blr x16");

    if (stack_size)
    {
        add_offset(cg, "sp", "sp", stack_size);
        cg->depth -= stack_size / 16;
    }
    free(pl);

    /* 5. the result */
    if (type_is_record(ret))
    {
        if (rc.kind == K_INT)
        {
            add_offset(cg, "x9", "x29", ret_tmp);
            emit(cg, "    str x0, [x9]");
            if (rc.n > 1) emit(cg, "    str x1, [x9, #8]");
        }
        else if (rc.kind == K_HFA)
        {
            add_offset(cg, "x9", "x29", ret_tmp);
            for (int j = 0; j < rc.n; j++)
                emit(cg, "    str %s, [x9, #%d]", rc.fsize == 4 ? sr[j] : dr[j], j * rc.fsize);
        }
        add_offset(cg, "x0", "x29", ret_tmp);
        return;
    }
    normalize(cg, ret);
}

/* -------------------------------------------------------------- va_list */

/*
 * Linux va_list:
 *     void* __stack;     next argument on the stack
 *     void* __gr_top;    end of the saved x registers
 *     void* __vr_top;    end of the saved v registers
 *     int   __gr_offs;   negative: offset from __gr_top of the next x register
 *     int   __vr_offs;   negative: offset from __vr_top of the next v register
 * macOS: a char* to the next stack slot (8 bytes each).
 */
static void gen_va_start(struct cg* cg, struct node* n)
{
    gen_expr(cg, n->lhs);       /* address of the va_list */
    emit(cg, "    mov x10, x0");
    add_offset(cg, "x9", "x29", 16 + cg->named_stack);
    emit(cg, "    str x9, [x10]");
    if (cg->apple)
        return;
    add_offset(cg, "x9", "x29", cg->gr_save_offset + 64);
    emit(cg, "    str x9, [x10, #8]");
    add_offset(cg, "x9", "x29", cg->vr_save_offset + 128);
    emit(cg, "    str x9, [x10, #16]");
    load_imm(cg, "x9", (unsigned)(-(8 - cg->named_gr) * 8));
    emit(cg, "    str w9, [x10, #24]");
    load_imm(cg, "x9", (unsigned)(-(8 - cg->named_vr) * 16));
    emit(cg, "    str w9, [x10, #28]");
}

static void gen_va_arg(struct cg* cg, struct node* n)
{
    struct type* t = n->type;
    struct arg_class c = classify(t);
    int size_on_stack = c.kind == K_INDIRECT ? 8 : align_to(t->size, 8);

    gen_expr(cg, n->lhs);
    emit(cg, "    mov x10, x0");

    int done = new_label(cg);
    if (!cg->apple)
    {
        int from_stack = new_label(cg);
        int fp = c.kind == K_FP || c.kind == K_HFA;
        int field = fp ? 28 : 24;           /* __vr_offs or __gr_offs */
        int step = fp ? 16 : 8;
        int top = fp ? 16 : 8;              /* __vr_top or __gr_top */

        /* offs >= 0: the registers are over; else take n registers */
        emit(cg, "    ldrsw x11, [x10, #%d]", field);
        emit(cg, "    cmp x11, #0");
        emit(cg, "    b.ge .L%d", from_stack);
        add_offset(cg, "x12", "x11", c.n * step);
        emit(cg, "    str w12, [x10, #%d]", field);
        emit(cg, "    cmp x12, #0");
        emit(cg, "    b.gt .L%d", from_stack);
        emit(cg, "    ldr x9, [x10, #%d]", top);
        emit(cg, "    add x0, x9, x11");     /* address of the first register */
        if (c.kind == K_HFA)
        {
            /* the members are in separate 16-byte slots: gather them */
            int tmp = alloc_temp(cg, 16, 16);
            add_offset(cg, "x1", "x29", tmp);
            for (int j = 0; j < c.n; j++)
            {
                emit(cg, c.fsize == 4 ? "    ldr w9, [x0, #%d]" : "    ldr x9, [x0, #%d]", j * 16);
                emit(cg, c.fsize == 4 ? "    str w9, [x1, #%d]" : "    str x9, [x1, #%d]", j * c.fsize);
            }
            emit(cg, "    mov x0, x1");
        }
        emit(cg, "    b .L%d", done);
        emit(cg, ".L%d:", from_stack);
    }

    /* from the stack: the address, then advance */
    emit(cg, "    ldr x0, [x10]");
    add_offset(cg, "x9", "x0", size_on_stack);
    emit(cg, "    str x9, [x10]");

    emit(cg, ".L%d:", done);
    if (c.kind == K_INDIRECT)
        emit(cg, "    ldr x0, [x0]");     /* the argument is a pointer to a copy */
    load(cg, t);
}

static void gen_expr(struct cg* cg, struct node* n)
{
    switch (n->kind)
    {
        case N_NUM:
        {
            long long v = (long long)n->ival;
            if (n->type->size < 8)
                v = n->type->is_unsigned ? (long long)(unsigned)v : (long long)(int)v;
            load_imm(cg, "x0", (unsigned long long)v);
            return;
        }
        case N_FNUM:
            if (n->type->kind == TY_FLOAT)
            {
                float f = (float)n->fval;
                unsigned bits;
                memcpy(&bits, &f, 4);
                load_imm(cg, "x0", bits);
                emit(cg, "    fmov s0, w0       // %g", n->fval);
            }
            else
            {
                unsigned long long bits;
                memcpy(&bits, &n->fval, 8);
                load_imm(cg, "x0", bits);
                emit(cg, "    fmov d0, x0       // %g", n->fval);
            }
            return;

        case N_VAR:
        case N_DEREF:
        case N_MEMBER:
            gen_addr(cg, n);
            load_lvalue(cg, n);
            return;

        case N_ADDR:
            gen_addr(cg, n->lhs);
            return;

        case N_CAST:
            gen_expr(cg, n->lhs);
            cast(cg, n->lhs->type, n->type);
            return;

        case N_NEG:
            gen_expr(cg, n->lhs);
            if (is_flt(n->type))
                emit(cg, "    fneg %s, %s", fpreg(n->type, 0), fpreg(n->type, 0));
            else
            {
                emit(cg, "    neg x0, x0");
                normalize(cg, n->type);
            }
            return;

        case N_BITNOT:
            gen_expr(cg, n->lhs);
            emit(cg, "    mvn x0, x0");
            normalize(cg, n->type);
            return;

        case N_NOT:
            gen_expr(cg, n->lhs);
            if (is_flt(n->lhs->type))
                emit(cg, "    fcmp %s, #0.0", fpreg(n->lhs->type, 0));
            else
                emit(cg, "    cmp x0, #0");
            emit(cg, "    cset x0, eq");
            return;

        case N_ADD: case N_SUB: case N_MUL: case N_DIV: case N_MOD:
        case N_SHL: case N_SHR: case N_BITAND: case N_BITOR: case N_BITXOR:
            gen_operands(cg, n->lhs, n->rhs);
            binary_op(cg, n->kind, n->type);
            return;

        case N_EQ: case N_NE: case N_LT: case N_LE:
            gen_operands(cg, n->lhs, n->rhs);
            compare_op(cg, n->kind, n->lhs->type);
            return;

        case N_LOGAND:
        case N_LOGOR:
        {
            int is_and = n->kind == N_LOGAND;
            int short_circuit = new_label(cg), end = new_label(cg);
            gen_expr(cg, n->lhs);
            branch_zero(cg, n->lhs->type, is_and, short_circuit);
            gen_expr(cg, n->rhs);
            branch_zero(cg, n->rhs->type, is_and, short_circuit);
            emit(cg, "    movz x0, #%d", is_and ? 1 : 0);
            emit(cg, "    b .L%d", end);
            emit(cg, ".L%d:", short_circuit);
            emit(cg, "    movz x0, #%d", is_and ? 0 : 1);
            emit(cg, ".L%d:", end);
            return;
        }

        case N_COND:
        {
            int els = new_label(cg), end = new_label(cg);
            gen_expr(cg, n->cond);
            branch_zero(cg, n->cond->type, 1, els);
            gen_expr(cg, n->then);
            emit(cg, "    b .L%d", end);
            emit(cg, ".L%d:", els);
            gen_expr(cg, n->els);
            emit(cg, ".L%d:", end);
            return;
        }

        case N_COMMA:
            gen_expr(cg, n->lhs);
            gen_expr(cg, n->rhs);
            return;

        case N_ASSIGN:
            gen_addr(cg, n->lhs);
            push_x(cg, "x0");
            gen_expr(cg, n->rhs);
            pop_x(cg, "x1");
            store_lvalue(cg, n->lhs);
            return;

        case N_ASSIGN_OP:
        {
            /* stack: address [old value] current value */
            struct type* lt = n->lhs->type;
            struct type* ot = n->op_type;
            gen_addr(cg, n->lhs);
            push_x(cg, "x0");
            load_lvalue(cg, n->lhs);
            if (n->is_post)
                push_value(cg, lt);
            cast(cg, lt, ot);
            push_value(cg, ot);
            gen_expr(cg, n->rhs);
            if (is_flt(n->rhs->type)) emit(cg, "    fmov %s, %s", fpreg(n->rhs->type, 1), fpreg(n->rhs->type, 0));
            else emit(cg, "    mov x1, x0");
            if (is_flt(ot)) pop_x(cg, "d0");
            else pop_x(cg, "x0");
            binary_op(cg, n->op, ot->kind == TY_PTR ? n->rhs->type : ot);
            cast(cg, ot, lt);
            if (n->is_post)
            {
                if (is_flt(lt)) pop_x(cg, "d2");
                else pop_x(cg, "x2");
            }
            pop_x(cg, "x1");
            store_lvalue(cg, n->lhs);
            if (n->is_post)
            {
                if (is_flt(lt)) emit(cg, "    fmov d0, d2");
                else emit(cg, "    mov x0, x2");
            }
            return;
        }

        case N_CALL:
            gen_call(cg, n);
            return;

        case N_VA_START:
            gen_va_start(cg, n);
            return;

        case N_VA_ARG:
            gen_va_arg(cg, n);
            return;
    }
    cc_error_at(n->tok, "internal error: expression not supported");
}

/* -------------------------------------------------------- statements */

static void gen_init(struct cg* cg, struct stmt* s)
{
    struct obj* v = s->var;
    struct type* t = v->type;

    if (!type_is_scalar(t))
    {
        /* aggregates: start with zeros */
        add_offset(cg, "x1", "x29", v->offset);
        int i = 0;
        for (; i + 8 <= t->size; i += 8)
        {
            if (i >= 32760) { add_offset(cg, "x1", "x1", i); t = t; }
            emit(cg, "    str xzr, [x1, #%d]", i < 32760 ? i : 0);
            if (i >= 32760) add_offset(cg, "x1", "x1", -i);
        }
        for (; i < t->size; i++)
        {
            add_offset(cg, "x9", "x1", i);
            emit(cg, "    strb wzr, [x9]");
        }
    }

    for (struct init_item* it = s->items; it; it = it->next)
    {
        gen_expr(cg, it->expr);
        add_offset(cg, "x1", "x29", v->offset + it->offset);
        if (it->bitfield) store_bitfield(cg, it->bitfield);
        else store(cg, it->type);
    }
}

/* the value of the return statement is in x0 / s0 / d0 */
static void gen_return_value(struct cg* cg, struct type* t)
{
    if (!type_is_record(t))
        return;
    struct arg_class c = classify(t);
    if (c.kind == K_INDIRECT)
    {
        /* copy to the memory the caller passed in x8 */
        add_offset(cg, "x1", "x29", cg->indirect_ret_offset);
        emit(cg, "    ldr x1, [x1]");
        copy_bytes(cg, t->size);
        emit(cg, "    mov x0, x1");
        return;
    }
    struct_to_temp(cg, t);
    emit(cg, "    mov x9, x0");
    if (c.kind == K_HFA)
    {
        static const char* dr[] = { "d0", "d1", "d2", "d3" };
        static const char* sr[] = { "s0", "s1", "s2", "s3" };
        for (int j = 0; j < c.n; j++)
            emit(cg, "    ldr %s, [x9, #%d]", c.fsize == 4 ? sr[j] : dr[j], j * c.fsize);
        return;
    }
    emit(cg, "    ldr x0, [x9]");
    if (c.n > 1) emit(cg, "    ldr x1, [x9, #8]");
}

static void gen_stmt(struct cg* cg, struct stmt* s)
{
    /* -g: the code from here on comes from this line */
    if (cg->dw && s->tok && s->kind != S_BLOCK)
        emit(cg, "    .loc %d %d", dwarf_file(cg->dw, s->tok->file), s->tok->line);

    switch (s->kind)
    {
        case S_BLOCK:
            for (struct stmt* x = s->body; x; x = x->next)
                gen_stmt(cg, x);
            return;

        case S_EXPR:
            if (s->expr)
                gen_expr(cg, s->expr);
            return;

        case S_INIT:
            gen_init(cg, s);
            return;

        case S_IF:
        {
            int els = new_label(cg), end = new_label(cg);
            gen_expr(cg, s->expr);
            branch_zero(cg, s->expr->type, 1, els);
            gen_stmt(cg, s->then);
            emit(cg, "    b .L%d", end);
            emit(cg, ".L%d:", els);
            if (s->els)
                gen_stmt(cg, s->els);
            emit(cg, ".L%d:", end);
            return;
        }

        case S_WHILE:
        case S_FOR:
        case S_DO:
        {
            int saved_break = cg->break_label, saved_continue = cg->continue_label;
            int top = new_label(cg);
            cg->break_label = new_label(cg);
            cg->continue_label = new_label(cg);

            if (s->kind == S_FOR && s->init)
                gen_expr(cg, s->init);

            emit(cg, ".L%d:", top);
            if (s->kind == S_DO)
            {
                gen_stmt(cg, s->body);
                emit(cg, ".L%d:", cg->continue_label);
                gen_expr(cg, s->expr);
                branch_zero(cg, s->expr->type, 0, top);
            }
            else
            {
                if (s->expr)
                {
                    gen_expr(cg, s->expr);
                    branch_zero(cg, s->expr->type, 1, cg->break_label);
                }
                gen_stmt(cg, s->body);
                emit(cg, ".L%d:", cg->continue_label);
                if (s->inc)
                    gen_expr(cg, s->inc);
                emit(cg, "    b .L%d", top);
            }
            emit(cg, ".L%d:", cg->break_label);
            cg->break_label = saved_break;
            cg->continue_label = saved_continue;
            return;
        }

        case S_GOTO:
            emit(cg, "    b .L.%s.%s", cg->fn->label, s->label);
            return;

        case S_LABEL:
            emit(cg, ".L.%s.%s:", cg->fn->label, s->label);
            gen_stmt(cg, s->body);
            return;

        case S_BREAK:
            emit(cg, "    b .L%d", cg->break_label);
            return;

        case S_CONTINUE:
            emit(cg, "    b .L%d", cg->continue_label);
            return;

        case S_RETURN:
            if (s->expr)
            {
                gen_expr(cg, s->expr);
                gen_return_value(cg, s->expr->type);
            }
            emit(cg, "    b .L%d", cg->return_label);
            return;
    }
}

/* ------------------------------------------------------------ functions */

/* where the parameters are, and the code that stores the argument registers */
static void gen_params(struct cg* cg, struct obj* fn)
{
    static const char* xr[] = { "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7" };
    static const char* dr[] = { "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7" };
    static const char* sr[] = { "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7" };
    struct text* p = &cg->prologue;
    int ngrn = 0, nsrn = 0, nsaa = 0;

    if (type_is_record(fn->type->base) && classify(fn->type->base).kind == K_INDIRECT)
    {
        cg->indirect_ret_offset = alloc_temp(cg, 8, 8);
        text_printf(p, "    sub x9, x29, #%d\n    str x8, [x9]          // where to write the result\n",
                    -cg->indirect_ret_offset);
    }

    for (struct obj* v = fn->params; v; v = v->next)
    {
        struct placement pl;
        place(cg, v->type, 1, &ngrn, &nsrn, &nsaa, &pl);
        if (pl.on_stack)
        {
            v->offset = 16 + pl.stack_offset;
            continue;
        }
        int size = pl.c.kind == K_INDIRECT ? 8 : v->type->size;
        v->offset = alloc_temp(cg, align_to(size, 8) < 8 * pl.c.n ? 8 * pl.c.n : align_to(size, 8), v->type->align);
        text_printf(p, "    sub x9, x29, #%d          // %s\n", -v->offset, v->name);
        for (int j = 0; j < pl.c.n; j++)
        {
            if (pl.c.kind == K_FP || pl.c.kind == K_HFA)
                text_printf(p, "    str %s, [x9, #%d]\n", pl.c.fsize == 4 ? sr[pl.reg + j] : dr[pl.reg + j], j * pl.c.fsize);
            else
                text_printf(p, "    str %s, [x9, #%d]\n", xr[pl.reg + j], j * 8);
        }
    }
    cg->named_gr = ngrn;
    cg->named_vr = nsrn;
    cg->named_stack = nsaa;

    if (fn->type->is_variadic && !cg->apple)
    {
        /* save every argument register for va_arg */
        cg->gr_save_offset = alloc_temp(cg, 64, 16);
        cg->vr_save_offset = alloc_temp(cg, 128, 16);
        text_printf(p, "    sub x9, x29, #%d\n", -cg->gr_save_offset);
        for (int i = 0; i < 8; i++)
            text_printf(p, "    str x%d, [x9, #%d]\n", i, 8 * i);
        text_printf(p, "    sub x9, x29, #%d\n", -cg->vr_save_offset);
        for (int i = 0; i < 8; i++)
            text_printf(p, "    str q%d, [x9, #%d]\n", i, 16 * i);
    }
}

static void gen_function(struct cg* cg, struct obj* fn)
{
    cg->fn = fn;
    cg->depth = 0;
    cg->frame_size = 0;
    cg->body.size = 0;
    cg->prologue.size = 0;
    cg->return_label = new_label(cg);

    gen_params(cg, fn);
    for (struct obj* v = fn->locals; v; v = v->next_local)
        if (v->param_index < 0)
            v->offset = alloc_temp(cg, v->type->size, v->type->align);

    gen_stmt(cg, fn->body);

    struct text* out = cg->out;
    text_printf(out, "\n    .text\n    .balign 4\n");
    if (!fn->is_static)
        text_printf(out, "    .globl %s\n", fn->label);
    text_printf(out, "    .func %s\n", fn->label);
    text_printf(out, "%s:\n", fn->label);
    if (cg->dw && fn->tok)
        text_printf(out, "    .loc %d %d\n", dwarf_file(cg->dw, fn->tok->file), fn->tok->line);
    text_printf(out, "    stp x29, x30, [sp, #-16]!\n");
    text_printf(out, "    mov x29, sp\n");
    int frame = align_to(cg->frame_size, 16);
    if (frame)
    {
        if (frame < 4096)
            text_printf(out, "    sub sp, sp, #%d\n", frame);
        else
        {
            text_printf(out, "    sub sp, sp, #%d, lsl #12\n", frame >> 12);
            if (frame & 0xFFF)
                text_printf(out, "    sub sp, sp, #%d\n", frame & 0xFFF);
        }
    }
    if (cg->prologue.size)
        text_append(out, cg->prologue.data, cg->prologue.size);
    text_append(out, cg->body.data, cg->body.size);
    text_printf(out, ".L%d:\n", cg->return_label);
    text_printf(out, "    mov sp, x29\n");
    text_printf(out, "    ldp x29, x30, [sp], #16\n");
    text_printf(out, "    ret\n");
    if (cg->dw)
        text_printf(out, ".Lend.%s:\n", fn->label);
    text_printf(out, "    .endfunc\n");
}

void backend_arm64(struct program* prog, struct text* out)
{
    struct cg cg = { 0 };
    cg.out = out;
    cg.apple = prog->target->kind == TARGET_MACOS_ARM64;

    text_printf(out, "// generated by cc89 -- %s\n", prog->target->name);
    if (prog->debug)
    {
        cg.dw = dwarf_new(out);
        text_printf(out, "    .text\n.Ldebug_text_begin:\n");
    }
    for (struct obj* v = prog->globals; v; v = v->next)
    {
        if (v->is_function)
        {
            if (v->is_defined && v->body)
                gen_function(&cg, v);
        }
        else if (v->is_defined)
            gen_data(out, v);
        else
            text_printf(out, "    .extern_data %s\n", v->label);
    }
    if (cg.dw)
    {
        text_printf(out, "    .text\n.Ldebug_text_end:\n");
        dwarf_emit(cg.dw, prog, 29 /* x29 */, ".Ldebug_text_begin", ".Ldebug_text_end");
    }
    free(cg.body.data);
    free(cg.prologue.data);
}
