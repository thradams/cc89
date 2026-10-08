/*
 * Microsoft x64 calling convention (Windows).
 *
 *   - the first 4 arguments go in rcx, rdx, r8, r9, or in xmm0-xmm3
 *     when they are float/double. Argument i always uses slot i.
 *   - the caller reserves 32 bytes ("home area" or shadow space) for
 *     those 4 arguments, and arguments 5.. go on the stack after it:
 *
 *            [rsp+0]  [rsp+8]  [rsp+16]  [rsp+24]  [rsp+32] ...
 *             arg0     arg1     arg2      arg3      arg4
 *
 *   - rsp is a multiple of 16 at the call instruction.
 *   - struct of size 1, 2, 4 or 8 is passed and returned by value in
 *     the register; any other size is passed as a pointer to a copy,
 *     and returned through a hidden pointer received in rcx.
 *   - in variadic calls a float value is passed in BOTH registers
 *     (the callee reads it from the integer one); here we always load
 *     both, which is simple and correct.
 *
 * In the callee:
 *
 *        [rbp+40]  home slot 3 (r9)
 *        [rbp+32]  home slot 2 (r8)
 *        [rbp+24]  home slot 1 (rdx)
 *        [rbp+16]  home slot 0 (rcx)
 *
 * The prologue stores the 4 argument registers in the home slots, so
 * every parameter has an address and va_start is just "&last + 8".
 */
#include "x64.h"

static int is_reg_size(int size)
{
    return size == 1 || size == 2 || size == 4 || size == 8;
}

/* struct/union passed as a pointer to a copy */
int win64_by_reference(struct type* t)
{
    return type_is_record(t) && !is_reg_size(t->size);
}

/* load size bytes from [rax] into rax (small struct passed by value) */
static void load_raw(struct cg* cg, int size)
{
    switch (size)
    {
        case 1: emit(cg, "    movzx eax, byte ptr [rax]"); break;
        case 2: emit(cg, "    movzx eax, word ptr [rax]"); break;
        case 4: emit(cg, "    mov eax, dword ptr [rax]"); break;
        default: emit(cg, "    mov rax, qword ptr [rax]"); break;
    }
}

void win64_params(struct cg* cg, struct obj* fn)
{
    /* parameters live in their home slots (after the hidden pointer) */
    cg->hidden_ret = win64_by_reference(fn->type->base);
    for (struct obj* v = fn->locals; v; v = v->next_local)
        if (v->param_index >= 0)
            v->offset = 16 + 8 * (v->param_index + cg->hidden_ret);
}

void win64_prologue(struct cg* cg, struct obj* fn)
{
    /* store the argument registers in the home slots */
    static const char* regs[] = { "rcx", "rdx", "r8", "r9" };
    struct param* pa = fn->type->params;
    for (int slot = 0; slot < 4; slot++)
    {
        struct type* t = NULL;
        if (slot >= cg->hidden_ret && pa)
        {
            t = pa->type;
            pa = pa->next;
        }
        if (t && is_flt(t))
            text_printf(&cg->prologue, "    movq qword ptr [rbp+%d], xmm%d\n", 16 + slot * 8, slot);
        else
            text_printf(&cg->prologue, "    mov qword ptr [rbp+%d], %s\n", 16 + slot * 8, regs[slot]);
    }
}

/* the value of the return statement is in rax/xmm0 */
void win64_return(struct cg* cg, struct type* t)
{
    if (win64_by_reference(t))
    {
        /* copy to the caller's memory, return its address */
        emit(cg, "    mov rcx, qword ptr [rbp+16]");
        copy_bytes(cg, t->size);
        emit(cg, "    mov rax, rcx");
    }
    else if (type_is_record(t))
        load_raw(cg, t->size);
}

void win64_call(struct cg* cg, struct node* n)
{
    struct type* ret = n->type;
    int hidden = win64_by_reference(ret);
    int nargs = n->nargs + hidden;
    int slots = nargs < 4 ? 4 : nargs;

    /* temporary for the returned struct */
    int ret_tmp = 0;
    if (type_is_record(ret))
        ret_tmp = alloc_temp(cg, ret->size < 8 ? 8 : ret->size, ret->align);

    /* rsp must be 16-aligned at the call: depth is even when aligned */
    int pad = (cg->depth + slots) % 2;
    if (pad)
    {
        emit(cg, "    sub rsp, 8");
        cg->depth++;
    }

    /* push from the last slot to the first, so that slot i ends at [rsp+8*i] */
    for (int i = slots - 1; i >= nargs; i--)
        push(cg, "0");

    for (int i = n->nargs - 1; i >= 0; i--)
    {
        struct node* arg = n->args[i];
        gen_expr(cg, arg);
        if (win64_by_reference(arg->type))
        {
            /* pass a pointer to a copy */
            int tmp = alloc_temp(cg, arg->type->size, arg->type->align);
            emit(cg, "    lea rcx, [rbp%+d]", tmp);
            copy_bytes(cg, arg->type->size);
            emit(cg, "    lea rax, [rbp%+d]", tmp);
        }
        else if (type_is_record(arg->type))
            load_raw(cg, arg->type->size);
        else if (is_flt(arg->type))
            emit(cg, "    movq rax, xmm0");
        push(cg, "rax");
    }
    if (hidden)
    {
        emit(cg, "    lea rax, [rbp%+d]", ret_tmp);
        push(cg, "rax");
    }

    /* the function: a direct call when it is a named function */
    struct node* fn = n->lhs;
    int direct = fn->kind == N_ADDR && fn->lhs->kind == N_VAR && fn->lhs->var->is_function;
    if (!direct)
    {
        gen_expr(cg, fn);
        emit(cg, "    mov r10, rax");
    }

    static const char* regs[] = { "rcx", "rdx", "r8", "r9" };
    for (int i = 0; i < 4; i++)
    {
        emit(cg, "    mov %s, qword ptr [rsp+%d]", regs[i], i * 8);
        emit(cg, "    movq xmm%d, qword ptr [rsp+%d]", i, i * 8);
    }

    if (direct)
        emit(cg, "    call %s", fn->lhs->var->label);
    else
        emit(cg, "    call r10");

    emit(cg, "    add rsp, %d", (slots + pad) * 8);
    cg->depth -= slots + pad;

    if (type_is_record(ret))
    {
        if (!hidden)
        {
            /* small struct came back in rax: keep it in memory */
            emit(cg, "    mov qword ptr [rbp%+d], rax", ret_tmp);
        }
        emit(cg, "    lea rax, [rbp%+d]", ret_tmp);
        return;
    }
    /* the callee leaves the upper bits of small types undefined */
    normalize(cg, ret);
}

/* va_list is a char* that walks the home slots and the stack arguments */
void win64_va_start(struct cg* cg, struct node* n)
{
    /* *ap = (char*)&last + 8: the next slot */
    gen_expr(cg, n->lhs);
    emit(cg, "    lea rcx, [rbp%+d]", n->rhs->var->offset + 8);
    emit(cg, "    mov qword ptr [rax], rcx");
}

void win64_va_arg(struct cg* cg, struct node* n)
{
    /* every argument takes 8 bytes; a big struct is a pointer to a copy */
    gen_expr(cg, n->lhs);
    emit(cg, "    mov r10, rax");
    emit(cg, "    mov rax, qword ptr [r10]");
    emit(cg, "    lea rcx, [rax+8]");
    emit(cg, "    mov qword ptr [r10], rcx");
    if (win64_by_reference(n->type))
        emit(cg, "    mov rax, qword ptr [rax]");
    load(cg, n->type);
}
