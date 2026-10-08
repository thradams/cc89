/*
 * Back-end: x64. Writes assembly text (Intel syntax) that asm_x64.c
 * turns into machine code.
 *
 * The code generator is a simple stack machine, written to be read:
 *
 *   - every expression leaves its value in one place:
 *       integers and pointers  -> rax (always extended to 64 bits
 *                                 according to the type)
 *       float, double          -> xmm0 (float in single precision)
 *       struct, union          -> rax holds the ADDRESS of the value
 *   - a binary operator evaluates the left side, pushes it, evaluates
 *     the right side and pops the left side back.
 *   - every local lives in memory, in the frame of the function.
 *
 * Only registers that a call may destroy are used (rax rcx rdx r8-r11
 * xmm0-xmm5 on both ABIs), so the prologue saves nothing but rbp.
 *
 * Frame of a function:
 *
 *        [rbp+16]  arguments on the stack (see the ABI files)
 *        [rbp+8]   return address
 *        [rbp]     saved rbp
 *        [rbp-n]   locals and temporaries
 *
 * Calls, parameters, returns and va_list belong to the ABI:
 * abi_win64.c (Windows) and abi_sysv.c (Linux).
 */
#include "x64.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static void gen_addr(struct cg* cg, struct node* n);
static void gen_stmt(struct cg* cg, struct stmt* s);

void emit(struct cg* cg, const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    text_append(&cg->body, buf, n);
    text_append(&cg->body, "\n", 1);
}

int new_label(struct cg* cg)
{
    return cg->label_count++;
}

int align_to(int n, int align)
{
    return (n + align - 1) / align * align;
}

/* reserve space in the frame, returns the offset from rbp */
int alloc_temp(struct cg* cg, int size, int align)
{
    if (align < 8) align = 8;
    cg->frame_size = align_to(cg->frame_size + size, align);
    return -cg->frame_size;
}

int is_flt(struct type* t)
{
    return type_is_float(t);
}

/* float is single precision; double and long double are 8 bytes */
const char* sfx(struct type* t)
{
    return t->kind == TY_FLOAT ? "ss" : "sd";
}

/* ------------------------------------------------------------ stack */

void push(struct cg* cg, const char* reg)
{
    emit(cg, "    push %s", reg);
    cg->depth++;
}

void pop(struct cg* cg, const char* reg)
{
    emit(cg, "    pop %s", reg);
    cg->depth--;
}

void push_xmm(struct cg* cg, int r)
{
    emit(cg, "    sub rsp, 8");
    emit(cg, "    movq qword ptr [rsp], xmm%d", r);
    cg->depth++;
}

void pop_xmm(struct cg* cg, int r)
{
    emit(cg, "    movq xmm%d, qword ptr [rsp]", r);
    emit(cg, "    add rsp, 8");
    cg->depth--;
}

/* push the current value (rax or xmm0) */
void push_value(struct cg* cg, struct type* t)
{
    if (is_flt(t)) push_xmm(cg, 0);
    else push(cg, "rax");
}

/* ------------------------------------------------------ load / store */

/* re-extend rax to 64 bits as the type says */
void normalize(struct cg* cg, struct type* t)
{
    if (!type_is_integer(t))
        return;
    switch (t->size)
    {
        case 1: emit(cg, t->is_unsigned ? "    movzx eax, al" : "    movsx rax, al"); break;
        case 2: emit(cg, t->is_unsigned ? "    movzx eax, ax" : "    movsx rax, ax"); break;
        case 4: emit(cg, t->is_unsigned ? "    mov eax, eax" : "    movsxd rax, eax"); break;
    }
}

/* rax has an address; replace it by the value stored there */
void load(struct cg* cg, struct type* t)
{
    if (t->kind == TY_ARRAY || t->kind == TY_FUNC || type_is_record(t))
        return; /* the address is the value */

    if (is_flt(t))
    {
        emit(cg, t->kind == TY_FLOAT ? "    movss xmm0, dword ptr [rax]" : "    movsd xmm0, qword ptr [rax]");
        return;
    }
    switch (t->size)
    {
        case 1: emit(cg, t->is_unsigned ? "    movzx eax, byte ptr [rax]" : "    movsx rax, byte ptr [rax]"); break;
        case 2: emit(cg, t->is_unsigned ? "    movzx eax, word ptr [rax]" : "    movsx rax, word ptr [rax]"); break;
        case 4: emit(cg, t->is_unsigned ? "    mov eax, dword ptr [rax]" : "    movsxd rax, dword ptr [rax]"); break;
        default: emit(cg, "    mov rax, qword ptr [rax]"); break;
    }
}

/* copy size bytes from [rax] to [rcx] using r11 */
void copy_bytes(struct cg* cg, int size)
{
    int i = 0;
    for (; i + 8 <= size; i += 8)
    {
        emit(cg, "    mov r11, qword ptr [rax+%d]", i);
        emit(cg, "    mov qword ptr [rcx+%d], r11", i);
    }
    for (; i + 4 <= size; i += 4)
    {
        emit(cg, "    mov r11d, dword ptr [rax+%d]", i);
        emit(cg, "    mov dword ptr [rcx+%d], r11d", i);
    }
    for (; i < size; i++)
    {
        emit(cg, "    mov r11b, byte ptr [rax+%d]", i);
        emit(cg, "    mov byte ptr [rcx+%d], r11b", i);
    }
}

/* store the value (rax / xmm0) at the address in rcx */
void store(struct cg* cg, struct type* t)
{
    if (type_is_record(t))
    {
        copy_bytes(cg, t->size);
        emit(cg, "    mov rax, rcx");
        return;
    }
    if (is_flt(t))
    {
        emit(cg, t->kind == TY_FLOAT ? "    movss dword ptr [rcx], xmm0" : "    movsd qword ptr [rcx], xmm0");
        return;
    }
    switch (t->size)
    {
        case 1: emit(cg, "    mov byte ptr [rcx], al"); break;
        case 2: emit(cg, "    mov word ptr [rcx], ax"); break;
        case 4: emit(cg, "    mov dword ptr [rcx], eax"); break;
        default: emit(cg, "    mov qword ptr [rcx], rax"); break;
    }
}

/*
 * Bit-fields. The member says where the storage unit is (offset) and
 * which bits are used (bit_offset, bit_width).
 *
 * read:  shift the field to the top of the register, then shift it
 *        back down: sar copies the sign bit, shr fills with zeros.
 */
static void load_bitfield(struct cg* cg, struct member* m)
{
    struct type unit = *m->type;
    unit.is_unsigned = 1;
    load(cg, &unit);
    emit(cg, "    shl rax, %d", 64 - m->bit_offset - m->bit_width);
    emit(cg, "    %s rax, %d", m->type->is_unsigned ? "shr" : "sar", 64 - m->bit_width);
}

/* write: unit = (unit & ~mask) | ((value << bit_offset) & mask)
   value in rax, address of the unit in rcx; rax is kept */
static void store_bitfield(struct cg* cg, struct member* m)
{
    unsigned long long mask = (m->bit_width == 64 ? ~0ull : (1ull << m->bit_width) - 1) << m->bit_offset;
    emit(cg, "    mov r8, rax");
    emit(cg, "    shl r8, %d", m->bit_offset);
    emit(cg, "    movabs r9, %llu", mask);
    emit(cg, "    and r8, r9");
    emit(cg, "    not r9");
    switch (m->type->size)
    {
        case 1: emit(cg, "    movzx r10d, byte ptr [rcx]"); break;
        case 2: emit(cg, "    movzx r10d, word ptr [rcx]"); break;
        case 4: emit(cg, "    mov r10d, dword ptr [rcx]"); break;
        default: emit(cg, "    mov r10, qword ptr [rcx]"); break;
    }
    emit(cg, "    and r10, r9");
    emit(cg, "    or r10, r8");
    switch (m->type->size)
    {
        case 1: emit(cg, "    mov byte ptr [rcx], r10b"); break;
        case 2: emit(cg, "    mov word ptr [rcx], r10w"); break;
        case 4: emit(cg, "    mov dword ptr [rcx], r10d"); break;
        default: emit(cg, "    mov qword ptr [rcx], r10"); break;
    }
}

static struct member* bitfield_of(struct node* n)
{
    return n->kind == N_MEMBER && n->member->is_bitfield ? n->member : NULL;
}

/* load the lvalue whose address is in rax */
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
        if (from->kind == TY_FLOAT && to->kind != TY_FLOAT) emit(cg, "    cvtss2sd xmm0, xmm0");
        if (from->kind != TY_FLOAT && to->kind == TY_FLOAT) emit(cg, "    cvtsd2ss xmm0, xmm0");
        return;
    }

    if (is_flt(to))
    {
        /* integer -> floating. rax already holds the exact 64-bit value,
           except for unsigned 64-bit values >= 2^63, which cvtsi2sd would
           read as negative: halve them (keeping the lowest bit), convert,
           and double the result. */
        if (from->size == 8 && from->is_unsigned)
        {
            int big = new_label(cg), end = new_label(cg);
            emit(cg, "    test rax, rax");
            emit(cg, "    js .L%d", big);
            emit(cg, "    cvtsi2%s xmm0, rax", sfx(to));
            emit(cg, "    jmp .L%d", end);
            emit(cg, ".L%d:", big);
            emit(cg, "    mov rcx, rax");
            emit(cg, "    shr rcx, 1");
            emit(cg, "    and eax, 1");
            emit(cg, "    or rcx, rax");
            emit(cg, "    cvtsi2%s xmm0, rcx", sfx(to));
            emit(cg, "    add%s xmm0, xmm0", sfx(to));
            emit(cg, ".L%d:", end);
            return;
        }
        emit(cg, "    cvtsi2%s xmm0, rax", sfx(to));
        return;
    }

    if (is_flt(from))
    {
        /* floating -> integer, truncating toward zero */
        emit(cg, "    cvtt%s2si rax, xmm0", sfx(from));
        normalize(cg, to);
        return;
    }

    /* integer/pointer -> integer/pointer: the 64-bit value in rax is
       already right; a smaller type only needs to be re-extended */
    if (to->size < 8)
        normalize(cg, to);
}

/* set the flags comparing the value with zero (ZF=1 when zero) */
static void cmp_zero(struct cg* cg, struct type* t)
{
    if (is_flt(t))
    {
        emit(cg, "    xorps xmm1, xmm1");
        emit(cg, "    ucomi%s xmm0, xmm1", sfx(t));
    }
    else
        emit(cg, "    cmp rax, 0");
}

/* ------------------------------------------------------- expressions */

static void gen_addr(struct cg* cg, struct node* n)
{
    switch (n->kind)
    {
        case N_VAR:
        {
            struct obj* v = n->var;
            if (v->is_local)
            {
                if (!cg->sysv && v->param_index >= 0 && win64_by_reference(v->type))
                    emit(cg, "    mov rax, qword ptr [rbp%+d]", v->offset);  /* slot holds a pointer */
                else
                    emit(cg, "    lea rax, [rbp%+d]", v->offset);
            }
            else if (cg->sysv && !v->is_defined && !v->is_function)
            {
                /* object of a shared library (stdout, errno...): its address
                   is in the GOT, filled by the dynamic loader */
                emit(cg, "    mov rax, qword ptr [rip+%s@GOT]", v->label);
            }
            else
                emit(cg, "    lea rax, [rip+%s]", v->label);
            return;
        }
        case N_DEREF:
            gen_expr(cg, n->lhs);
            return;
        case N_MEMBER:
            gen_addr(cg, n->lhs);
            if (n->member->offset)
                emit(cg, "    add rax, %d", n->member->offset);
            return;
        default:
            /* a struct value (call, ?:, =) is already an address */
            if (type_is_record(n->type))
            {
                gen_expr(cg, n);
                return;
            }
            break;
    }
    cc_error_at(n->tok, "not an lvalue");
}

/* integer or floating binary operation: left in rax/xmm0, right in rcx/xmm1 */
static void binary_op(struct cg* cg, enum node_kind op, struct type* t)
{
    if (is_flt(t))
    {
        const char* s = sfx(t);
        switch (op)
        {
            case N_ADD: emit(cg, "    add%s xmm0, xmm1", s); return;
            case N_SUB: emit(cg, "    sub%s xmm0, xmm1", s); return;
            case N_MUL: emit(cg, "    mul%s xmm0, xmm1", s); return;
            case N_DIV: emit(cg, "    div%s xmm0, xmm1", s); return;
            default: break;
        }
        return;
    }

    /* after the promotions an integer operation is 32 or 64 bits */
    int is64 = t->size == 8;
    const char* a = is64 ? "rax" : "eax";
    const char* c = is64 ? "rcx" : "ecx";
    const char* d = is64 ? "rdx" : "edx";
    switch (op)
    {
        case N_ADD: emit(cg, "    add %s, %s", a, c); break;
        case N_SUB: emit(cg, "    sub %s, %s", a, c); break;
        case N_MUL: emit(cg, "    imul %s, %s", a, c); break;
        case N_BITAND: emit(cg, "    and %s, %s", a, c); break;
        case N_BITOR: emit(cg, "    or %s, %s", a, c); break;
        case N_BITXOR: emit(cg, "    xor %s, %s", a, c); break;
        case N_SHL: emit(cg, "    shl %s, cl", a); break;
        case N_SHR: emit(cg, "    %s %s, cl", t->is_unsigned ? "shr" : "sar", a); break;
        case N_DIV:
        case N_MOD:
            /* dividend is edx:eax (rdx:rax): sign-extend it with cdq/cqo,
               or clear edx for unsigned */
            if (t->is_unsigned)
            {
                emit(cg, "    xor edx, edx");
                emit(cg, "    div %s", c);
            }
            else
            {
                emit(cg, is64 ? "    cqo" : "    cdq");
                emit(cg, "    idiv %s", c);
            }
            if (op == N_MOD)
                emit(cg, "    mov %s, %s", a, d);
            break;
        default:
            break;
    }
    normalize(cg, t);
}

/* left in rax/xmm0, right in rcx/xmm1; result 0 or 1 in rax */
static void compare_op(struct cg* cg, enum node_kind op, struct type* t)
{
    if (is_flt(t))
    {
        const char* s = sfx(t);
        /* ucomis sets the flags like an unsigned compare; with NaN
           (unordered) ZF, PF and CF are all 1, so '<' is written as
           "right above left", which is false for NaN */
        switch (op)
        {
            case N_LT: emit(cg, "    ucomi%s xmm1, xmm0", s); emit(cg, "    seta al"); break;
            case N_LE: emit(cg, "    ucomi%s xmm1, xmm0", s); emit(cg, "    setae al"); break;
            case N_EQ:
                emit(cg, "    ucomi%s xmm0, xmm1", s);
                emit(cg, "    sete al");
                emit(cg, "    setnp cl");
                emit(cg, "    and al, cl");
                break;
            default: /* N_NE */
                emit(cg, "    ucomi%s xmm0, xmm1", s);
                emit(cg, "    setne al");
                emit(cg, "    setp cl");
                emit(cg, "    or al, cl");
                break;
        }
        emit(cg, "    movzx eax, al");
        return;
    }

    /* both values are extended to 64 bits, so a 64-bit compare works */
    emit(cg, "    cmp rax, rcx");
    int u = t->is_unsigned || t->kind == TY_PTR;
    switch (op)
    {
        case N_EQ: emit(cg, "    sete al"); break;
        case N_NE: emit(cg, "    setne al"); break;
        case N_LT: emit(cg, u ? "    setb al" : "    setl al"); break;
        default:   emit(cg, u ? "    setbe al" : "    setle al"); break;
    }
    emit(cg, "    movzx eax, al");
}

/* evaluate a binary operator: left pushed, right in rcx/xmm1, left back in rax/xmm0 */
static void gen_operands(struct cg* cg, struct node* lhs, struct node* rhs)
{
    gen_expr(cg, lhs);
    push_value(cg, lhs->type);
    gen_expr(cg, rhs);
    if (is_flt(rhs->type)) emit(cg, "    movaps xmm1, xmm0");
    else emit(cg, "    mov rcx, rax");
    if (is_flt(lhs->type)) pop_xmm(cg, 0);
    else pop(cg, "rax");
}

void gen_expr(struct cg* cg, struct node* n)
{
    switch (n->kind)
    {
        case N_NUM:
        {
            long long v = (long long)n->ival;
            if (n->type->size < 8)
                v = n->type->is_unsigned ? (long long)(unsigned)v : (long long)(int)v;
            if (v >= -2147483648LL && v <= 2147483647LL)
                emit(cg, "    mov rax, %lld", v);
            else
                emit(cg, "    movabs rax, %lld", v);
            return;
        }
        case N_FNUM:
            if (n->type->kind == TY_FLOAT)
            {
                float f = (float)n->fval;
                unsigned bits;
                memcpy(&bits, &f, 4);
                emit(cg, "    mov eax, %u   # %g", bits, n->fval);
                emit(cg, "    movd xmm0, eax");
            }
            else
            {
                unsigned long long bits;
                memcpy(&bits, &n->fval, 8);
                emit(cg, "    movabs rax, %llu   # %g", bits, n->fval);
                emit(cg, "    movq xmm0, rax");
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
            if (n->type->kind == TY_FLOAT)
            {
                /* flip the sign bit */
                emit(cg, "    movd eax, xmm0");
                emit(cg, "    xor eax, 0x80000000");
                emit(cg, "    movd xmm0, eax");
            }
            else if (is_flt(n->type))
            {
                emit(cg, "    movq rax, xmm0");
                emit(cg, "    btc rax, 63");
                emit(cg, "    movq xmm0, rax");
            }
            else
            {
                emit(cg, "    neg rax");
                normalize(cg, n->type);
            }
            return;

        case N_BITNOT:
            gen_expr(cg, n->lhs);
            emit(cg, "    not rax");
            normalize(cg, n->type);
            return;

        case N_NOT:
            gen_expr(cg, n->lhs);
            cmp_zero(cg, n->lhs->type);
            emit(cg, "    sete al");
            emit(cg, "    movzx eax, al");
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
            /* a && b: if a is 0 the result is 0 and b is not evaluated */
            int is_and = n->kind == N_LOGAND;
            int short_circuit = new_label(cg), end = new_label(cg);
            gen_expr(cg, n->lhs);
            cmp_zero(cg, n->lhs->type);
            emit(cg, "    %s .L%d", is_and ? "je" : "jne", short_circuit);
            gen_expr(cg, n->rhs);
            cmp_zero(cg, n->rhs->type);
            emit(cg, "    %s .L%d", is_and ? "je" : "jne", short_circuit);
            emit(cg, "    mov rax, %d", is_and ? 1 : 0);
            emit(cg, "    jmp .L%d", end);
            emit(cg, ".L%d:", short_circuit);
            emit(cg, "    mov rax, %d", is_and ? 0 : 1);
            emit(cg, ".L%d:", end);
            return;
        }

        case N_COND:
        {
            int els = new_label(cg), end = new_label(cg);
            gen_expr(cg, n->cond);
            cmp_zero(cg, n->cond->type);
            emit(cg, "    je .L%d", els);
            gen_expr(cg, n->then);
            emit(cg, "    jmp .L%d", end);
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
            push(cg, "rax");
            gen_expr(cg, n->rhs);
            pop(cg, "rcx");
            store_lvalue(cg, n->lhs);
            return;

        case N_ASSIGN_OP:
        {
            /*
             * lhs op= rhs  (and ++/--). The address is computed once:
             *     stack: address [old value] current value
             */
            struct type* lt = n->lhs->type;
            struct type* ot = n->op_type;
            gen_addr(cg, n->lhs);
            push(cg, "rax");
            load_lvalue(cg, n->lhs);
            if (n->is_post)
                push_value(cg, lt);
            cast(cg, lt, ot);
            push_value(cg, ot);
            gen_expr(cg, n->rhs);
            if (is_flt(n->rhs->type)) emit(cg, "    movaps xmm1, xmm0");
            else emit(cg, "    mov rcx, rax");
            if (is_flt(ot)) pop_xmm(cg, 0);
            else pop(cg, "rax");
            binary_op(cg, n->op, ot->kind == TY_PTR ? n->rhs->type : ot);
            cast(cg, ot, lt);
            if (n->is_post)
            {
                if (is_flt(lt)) pop_xmm(cg, 2);
                else pop(cg, "rdx");
            }
            pop(cg, "rcx");
            store_lvalue(cg, n->lhs);
            if (n->is_post)
            {
                if (is_flt(lt)) emit(cg, "    movaps xmm0, xmm2");
                else emit(cg, "    mov rax, rdx");
            }
            return;
        }

        case N_CALL:
            if (cg->sysv) sysv_call(cg, n);
            else win64_call(cg, n);
            return;

        case N_VA_START:
            if (cg->sysv) sysv_va_start(cg, n);
            else win64_va_start(cg, n);
            return;

        case N_VA_ARG:
            if (cg->sysv) sysv_va_arg(cg, n);
            else win64_va_arg(cg, n);
            return;
    }
    cc_error_at(n->tok, "internal error: expression not supported");
}

/* -------------------------------------------------------- statements */

static void gen_init(struct cg* cg, struct stmt* s)
{
    struct obj* v = s->var;
    struct type* t = v->type;

    /* aggregates: start with zeros, members without initializer stay 0 */
    if (!type_is_scalar(t))
    {
        int i = 0;
        for (; i + 8 <= t->size; i += 8)
            emit(cg, "    mov qword ptr [rbp%+d], 0", v->offset + i);
        for (; i < t->size; i++)
            emit(cg, "    mov byte ptr [rbp%+d], 0", v->offset + i);
    }

    for (struct init_item* it = s->items; it; it = it->next)
    {
        gen_expr(cg, it->expr);
        emit(cg, "    lea rcx, [rbp%+d]", v->offset + it->offset);
        if (it->bitfield) store_bitfield(cg, it->bitfield);
        else store(cg, it->type);
    }
}

/* -g: number of a source file in the line table */
static int debug_file(struct cg* cg, const char* name)
{
    return cg->dw ? dwarf_file(cg->dw, name) : codeview_file(cg->cv, name);
}

static void gen_stmt(struct cg* cg, struct stmt* s)
{
    /* -g: the code from here on comes from this line */
    if ((cg->dw || cg->cv) && s->tok && s->kind != S_BLOCK)
        emit(cg, "    .loc %d %d", debug_file(cg, s->tok->file), s->tok->line);

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
            cmp_zero(cg, s->expr->type);
            emit(cg, "    je .L%d", els);
            gen_stmt(cg, s->then);
            emit(cg, "    jmp .L%d", end);
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

            if (s->kind == S_DO)
            {
                emit(cg, ".L%d:", top);
                gen_stmt(cg, s->body);
                emit(cg, ".L%d:", cg->continue_label);
                gen_expr(cg, s->expr);
                cmp_zero(cg, s->expr->type);
                emit(cg, "    jne .L%d", top);
            }
            else
            {
                emit(cg, ".L%d:", top);
                if (s->expr)
                {
                    gen_expr(cg, s->expr);
                    cmp_zero(cg, s->expr->type);
                    emit(cg, "    je .L%d", cg->break_label);
                }
                gen_stmt(cg, s->body);
                emit(cg, ".L%d:", cg->continue_label);
                if (s->inc)
                    gen_expr(cg, s->inc);
                emit(cg, "    jmp .L%d", top);
            }
            emit(cg, ".L%d:", cg->break_label);
            cg->break_label = saved_break;
            cg->continue_label = saved_continue;
            return;
        }

        case S_GOTO:
            emit(cg, "    jmp .L.%s.%s", cg->fn->label, s->label);
            return;

        case S_LABEL:
            emit(cg, ".L.%s.%s:", cg->fn->label, s->label);
            gen_stmt(cg, s->body);
            return;

        case S_BREAK:
            emit(cg, "    jmp .L%d", cg->break_label);
            return;

        case S_CONTINUE:
            emit(cg, "    jmp .L%d", cg->continue_label);
            return;

        case S_RETURN:
            if (s->expr)
            {
                gen_expr(cg, s->expr);
                if (cg->sysv) sysv_return(cg, s->expr->type);
                else win64_return(cg, s->expr->type);
            }
            emit(cg, "    jmp .L%d", cg->return_label);
            return;
    }
}

/* ------------------------------------------------------------ functions */

static void gen_function(struct cg* cg, struct obj* fn)
{
    cg->fn = fn;
    cg->depth = 0;
    cg->frame_size = 0;
    cg->body.size = 0;
    cg->prologue.size = 0;
    cg->return_label = new_label(cg);

    /* the ABI says where each parameter is; the other locals go in the frame */
    if (cg->sysv) sysv_params(cg, fn);
    else win64_params(cg, fn);
    for (struct obj* v = fn->locals; v; v = v->next_local)
        if (v->param_index < 0)
            v->offset = alloc_temp(cg, v->type->size, v->type->align);

    gen_stmt(cg, fn->body);

    /* the ABI part of the prologue: save the argument registers */
    if (cg->sysv) sysv_prologue(cg, fn);
    else win64_prologue(cg, fn);

    struct text* out = cg->out;
    text_printf(out, "\n    .text\n");
    if (!fn->is_static)
        text_printf(out, "    .globl %s\n", fn->label);
    /* .func/.endfunc: the linker needs the code range of each function
       (unwind tables on Windows, symbols and debug information) */
    text_printf(out, "    .func %s\n", fn->label);
    text_printf(out, "%s:\n", fn->label);
    if ((cg->dw || cg->cv) && fn->tok)
        text_printf(out, "    .loc %d %d\n", debug_file(cg, fn->tok->file), fn->tok->line);
    text_printf(out, "    push rbp\n");
    text_printf(out, "    mov rbp, rsp\n");
    int frame = align_to(cg->frame_size, 16);
    if (frame)
        text_printf(out, "    sub rsp, %d\n", frame);
    if (cg->prologue.size)
        text_append(out, cg->prologue.data, cg->prologue.size);

    text_append(out, cg->body.data, cg->body.size);

    text_printf(out, ".L%d:\n", cg->return_label);
    text_printf(out, "    mov rsp, rbp\n");
    text_printf(out, "    pop rbp\n");
    text_printf(out, "    ret\n");
    if (cg->dw)
        text_printf(out, ".Lend.%s:\n", fn->label);
    text_printf(out, "    .endfunc\n");
}

/* ---------------------------------------------------------------- data */

/*
 * MSVC intrinsics: MSVC expands them inline, no DLL exports them.
 * Here they are small functions (rcx = pointer, rdx = new value,
 * r8 = expected value). cmpxchg compares the accumulator with the
 * memory: if equal it stores the new value, and in both cases the
 * accumulator ends with the old value, which is the result.
 */
static const struct
{
    const char* name;
    const char* body;
} intrinsics[] = {
    { "_InterlockedCompareExchange8",  "    mov eax, r8d\n    lock cmpxchg byte ptr [rcx], dl\n    ret\n" },
    { "_InterlockedCompareExchange16", "    mov eax, r8d\n    lock cmpxchg word ptr [rcx], dx\n    ret\n" },
    { "_InterlockedCompareExchange",   "    mov eax, r8d\n    lock cmpxchg dword ptr [rcx], edx\n    ret\n" },
    { "_InterlockedCompareExchange64", "    mov rax, r8\n    lock cmpxchg qword ptr [rcx], rdx\n    ret\n" },
};

static void gen_intrinsics(struct program* prog, struct text* out)
{
    if (prog->target->kind != TARGET_WIN64)
        return;
    for (int i = 0; i < (int)(sizeof intrinsics / sizeof intrinsics[0]); i++)
    {
        int used = 0;
        for (struct obj* v = prog->globals; v; v = v->next)
            if (v->is_function && !v->body && strcmp(v->name, intrinsics[i].name) == 0)
                used = 1;
        if (used)
            text_printf(out, "\n    .text\n%s:\n%s", intrinsics[i].name, intrinsics[i].body);
    }
}

void backend_x64(struct program* prog, struct text* out)
{
    struct cg cg = { 0 };
    cg.out = out;
    cg.sysv = prog->target->kind != TARGET_WIN64;

    text_printf(out, "# generated by cc89 -- %s\n", prog->target->name);
    if (prog->debug && cg.sysv)
    {
        cg.dw = dwarf_new(out);
        text_printf(out, "    .text\n.Ldebug_text_begin:\n");
    }
    else if (prog->debug)
        cg.cv = codeview_new(out);

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
            text_printf(out, "    .extern_data %s\n", v->label);   /* object defined elsewhere */
    }
    gen_intrinsics(prog, out);
    if (cg.dw)
    {
        text_printf(out, "    .text\n.Ldebug_text_end:\n");
        dwarf_emit(cg.dw, prog, 6 /* rbp */, ".Ldebug_text_begin", ".Ldebug_text_end");
    }
    if (cg.cv)
        codeview_emit(cg.cv, prog, 334 /* CV_AMD64_RBP */);
    free(cg.body.data);
    free(cg.prologue.data);
}
