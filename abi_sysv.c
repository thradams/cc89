/*
 * System V AMD64 calling convention (Linux).
 *
 *   - integer and pointer arguments: rdi, rsi, rdx, rcx, r8, r9
 *   - float and double arguments:    xmm0 .. xmm7
 *     The two lists are used independently: f(int a, double b, int c)
 *     passes a in rdi, b in xmm0 and c in rsi.
 *   - what does not fit goes on the stack, in order, 8 bytes each
 *     (rounded up), starting at [rsp] at the call. No home area.
 *   - rsp is a multiple of 16 at the call instruction.
 *   - variadic calls: al = number of xmm registers used.
 *
 * Structs and unions are split in "eightbytes" (8-byte pieces):
 *   - bigger than 16 bytes: class MEMORY, copied on the stack.
 *   - otherwise each eightbyte is SSE when it only holds float/double,
 *     else INTEGER, and travels in the next register of its class.
 *     If the registers are not enough, the whole struct goes on the stack.
 *   - returned structs: INTEGER pieces in rax, rdx; SSE in xmm0, xmm1;
 *     MEMORY: the caller passes a hidden pointer in rdi, the callee
 *     returns it in rax.
 *
 * In the callee the argument registers are stored in the frame, so
 * every parameter has an address like any other local.
 *
 * va_list is a struct:
 *     unsigned gp_offset;        next integer register in the save area
 *     unsigned fp_offset;        next xmm register in the save area
 *     void* overflow_arg_area;   next argument on the stack
 *     void* reg_save_area;       rdi..r9 (48 bytes) then xmm0..xmm7 (16 each)
 * A variadic function saves all argument registers in its save area.
 */
#include "x64.h"

static const char* gp_regs[] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };

enum { C_INT = 1, C_SSE = 2 };

static void check_type(struct type* t)
{
    if (t->kind == TY_LDOUBLE)
        cc_error(NULL, 0, 0, "long double (x87) is not supported on linux-x64");
}

/* mark the class of each eightbyte touched by an object of type t at offset */
static void classify_at(struct type* t, int offset, int cls[2])
{
    if (type_is_record(t))
    {
        for (struct member* m = t->members; m; m = m->next)
            classify_at(m->type, offset + m->offset, cls);
        return;
    }
    if (t->kind == TY_ARRAY)
    {
        for (int i = 0; i < t->array_len; i++)
            classify_at(t->base, offset + i * t->base->size, cls);
        return;
    }
    int eb = offset / 8;
    if (eb < 2)
        cls[eb] |= is_flt(t) ? C_SSE : C_INT;
}

/* number of eightbytes passed in registers (1 or 2), or 0 for MEMORY */
static int classify(struct type* t, int cls[2])
{
    check_type(t);
    cls[0] = cls[1] = 0;
    if (!type_is_record(t))
    {
        cls[0] = is_flt(t) ? C_SSE : C_INT;
        return 1;
    }
    if (t->size > 16 || t->size == 0)
        return 0;
    classify_at(t, 0, cls);
    int n = (t->size + 7) / 8;
    for (int i = 0; i < n; i++)
        cls[i] = (cls[i] & C_INT) || cls[i] == 0 ? C_INT : C_SSE;
    return n;
}

static void count_classes(int n, int cls[2], int* ni, int* ns)
{
    *ni = *ns = 0;
    for (int i = 0; i < n; i++)
    {
        if (cls[i] == C_INT) (*ni)++;
        else (*ns)++;
    }
}

/* the struct at [rax] is copied to a temporary of whole eightbytes, so
   that its pieces can always be read with 8-byte loads; returns the
   temporary's offset and leaves its address in r11 */
static int struct_to_temp(struct cg* cg, struct type* t)
{
    int tmp = alloc_temp(cg, 16, 8);
    emit(cg, "    lea rcx, [rbp%+d]", tmp);
    copy_bytes(cg, t->size);
    emit(cg, "    lea r11, [rbp%+d]", tmp);
    return tmp;
}

/* ------------------------------------------------------------- callee */

void sysv_params(struct cg* cg, struct obj* fn)
{
    struct text* p = &cg->prologue;
    struct type* ret = fn->type->base;
    int cls[2];
    int gp = 0, fp = 0, stack = 16;

    cg->hidden_ret = type_is_record(ret) && classify(ret, cls) == 0;
    if (cg->hidden_ret)
    {
        cg->ret_ptr_offset = alloc_temp(cg, 8, 8);
        text_printf(p, "    mov qword ptr [rbp%+d], rdi       # hidden return pointer\n", cg->ret_ptr_offset);
        gp = 1;
    }

    for (struct obj* v = fn->params; v; v = v->next)
    {
        struct type* t = v->type;
        int n = classify(t, cls), ni, ns;
        count_classes(n, cls, &ni, &ns);

        if (n > 0 && gp + ni <= 6 && fp + ns <= 8)
        {
            /* arrived in registers: store them in the frame */
            v->offset = alloc_temp(cg, n * 8, t->align > 8 ? t->align : 8);
            for (int i = 0; i < n; i++)
            {
                if (cls[i] == C_INT)
                    text_printf(p, "    mov qword ptr [rbp%+d], %s       # %s\n", v->offset + 8 * i, gp_regs[gp++], v->name);
                else
                    text_printf(p, "    movq qword ptr [rbp%+d], xmm%d       # %s\n", v->offset + 8 * i, fp++, v->name);
            }
        }
        else
        {
            /* arrived on the stack, above the return address */
            v->offset = stack;
            stack += align_to(t->size, 8);
        }
    }

    if (fn->type->is_variadic)
    {
        /* register save area: every argument register, for va_arg */
        cg->reg_save_offset = alloc_temp(cg, 176, 16);
        for (int i = 0; i < 6; i++)
            text_printf(p, "    mov qword ptr [rbp%+d], %s\n", cg->reg_save_offset + 8 * i, gp_regs[i]);
        for (int i = 0; i < 8; i++)
            text_printf(p, "    movq qword ptr [rbp%+d], xmm%d\n", cg->reg_save_offset + 48 + 16 * i, i);
        cg->va_gp_offset = gp * 8;
        cg->va_fp_offset = 48 + fp * 16;
        cg->va_overflow_offset = stack;
    }
}

void sysv_prologue(struct cg* cg, struct obj* fn)
{
    /* everything was written by sysv_params */
    (void)cg;
    (void)fn;
}

/* the value of the return statement is in rax/xmm0 (address for structs) */
void sysv_return(struct cg* cg, struct type* t)
{
    int cls[2];
    if (!type_is_record(t))
    {
        check_type(t);
        return;
    }
    int n = classify(t, cls);
    if (n == 0)
    {
        /* MEMORY: copy to the caller's memory, return its address */
        emit(cg, "    mov rcx, qword ptr [rbp%+d]", cg->ret_ptr_offset);
        copy_bytes(cg, t->size);
        emit(cg, "    mov rax, rcx");
        return;
    }
    struct_to_temp(cg, t);
    int gi = 0, si = 0;
    for (int i = 0; i < n; i++)
    {
        if (cls[i] == C_INT)
            emit(cg, "    mov %s, qword ptr [r11+%d]", gi++ == 0 ? "rax" : "rdx", 8 * i);
        else
            emit(cg, "    movq xmm%d, qword ptr [r11+%d]", si++, 8 * i);
    }
}

/* ------------------------------------------------------------- caller */

void sysv_call(struct cg* cg, struct node* n)
{
    struct type* ret = n->type;
    int cls[2], ncls[256], rcls[256][2], on_stack[256];
    int ret_n = type_is_record(ret) ? classify(ret, cls) : 1;
    int hidden = type_is_record(ret) && ret_n == 0;
    int ret_cls[2] = { cls[0], cls[1] };

    int ret_tmp = 0;
    if (type_is_record(ret))
        ret_tmp = alloc_temp(cg, align_to(ret->size, 8) < 16 ? 16 : align_to(ret->size, 8), ret->align > 8 ? ret->align : 8);
    else
        check_type(ret);

    /* 1. decide where each argument goes */
    int gp = hidden, fp = 0, stack_bytes = 0;
    for (int i = 0; i < n->nargs; i++)
    {
        struct type* t = n->args[i]->type;
        int k = classify(t, rcls[i]), ni, ns;
        count_classes(k, rcls[i], &ni, &ns);
        ncls[i] = k;
        on_stack[i] = !(k > 0 && gp + ni <= 6 && fp + ns <= 8);
        if (on_stack[i])
            stack_bytes += align_to(t->size, 8);
        else
        {
            gp += ni;
            fp += ns;
        }
    }
    int nsse = fp;

    /* 2. stack arguments, last first, so the first ends at [rsp];
          padding first so that rsp is 16-aligned at the call */
    int pad = (cg->depth + stack_bytes / 8) % 2;
    if (pad)
    {
        emit(cg, "    sub rsp, 8");
        cg->depth++;
    }
    for (int i = n->nargs - 1; i >= 0; i--)
    {
        if (!on_stack[i])
            continue;
        struct node* arg = n->args[i];
        gen_expr(cg, arg);
        if (type_is_record(arg->type))
        {
            int size = align_to(arg->type->size, 8);
            emit(cg, "    sub rsp, %d", size);
            cg->depth += size / 8;
            emit(cg, "    mov rcx, rsp");
            copy_bytes(cg, arg->type->size);
        }
        else
            push_value(cg, arg->type);
    }

    /* 3. the function pointer, when it is not a named function */
    struct node* fn = n->lhs;
    int direct = fn->kind == N_ADDR && fn->lhs->kind == N_VAR && fn->lhs->var->is_function;
    if (!direct)
    {
        gen_expr(cg, fn);
        push(cg, "rax");
    }

    /* 4. register arguments: evaluate and push every piece, last first ... */
    for (int i = n->nargs - 1; i >= 0; i--)
    {
        if (on_stack[i])
            continue;
        struct node* arg = n->args[i];
        gen_expr(cg, arg);
        if (type_is_record(arg->type))
        {
            struct_to_temp(cg, arg->type);
            for (int j = ncls[i] - 1; j >= 0; j--)
            {
                emit(cg, "    mov r10, qword ptr [r11+%d]", 8 * j);
                push(cg, "r10");
            }
        }
        else
            push_value(cg, arg->type);
    }

    /* ... then pop them into their registers, first first */
    int gi = hidden, si = 0;
    for (int i = 0; i < n->nargs; i++)
    {
        if (on_stack[i])
            continue;
        for (int j = 0; j < ncls[i]; j++)
        {
            if (rcls[i][j] == C_INT) pop(cg, gp_regs[gi++]);
            else pop_xmm(cg, si++);
        }
    }
    if (hidden)
        emit(cg, "    lea rdi, [rbp%+d]", ret_tmp);
    if (!direct)
        pop(cg, "r11");

    emit(cg, "    mov eax, %d", nsse);     /* used by variadic functions */
    if (direct)
        emit(cg, "    call %s", fn->lhs->var->label);
    else
        emit(cg, "    call r11");

    if (stack_bytes + pad * 8)
        emit(cg, "    add rsp, %d", stack_bytes + pad * 8);
    cg->depth -= stack_bytes / 8 + pad;

    /* 5. the result */
    if (type_is_record(ret))
    {
        if (!hidden)
        {
            int gi2 = 0, si2 = 0;
            for (int j = 0; j < ret_n; j++)
            {
                if (ret_cls[j] == C_INT)
                    emit(cg, "    mov qword ptr [rbp%+d], %s", ret_tmp + 8 * j, gi2++ == 0 ? "rax" : "rdx");
                else
                    emit(cg, "    movq qword ptr [rbp%+d], xmm%d", ret_tmp + 8 * j, si2++);
            }
        }
        emit(cg, "    lea rax, [rbp%+d]", ret_tmp);
        return;
    }
    normalize(cg, ret);
}

/* -------------------------------------------------------------- va_list */

void sysv_va_start(struct cg* cg, struct node* n)
{
    gen_expr(cg, n->lhs);    /* address of the va_list struct */
    emit(cg, "    mov dword ptr [rax], %d", cg->va_gp_offset);
    emit(cg, "    mov dword ptr [rax+4], %d", cg->va_fp_offset);
    emit(cg, "    lea rcx, [rbp%+d]", cg->va_overflow_offset);
    emit(cg, "    mov qword ptr [rax+8], rcx");
    emit(cg, "    lea rcx, [rbp%+d]", cg->reg_save_offset);
    emit(cg, "    mov qword ptr [rax+16], rcx");
}

/* rax = address of the next piece in the save area; advances the offset */
static void next_saved_register(struct cg* cg, int is_sse)
{
    int field = is_sse ? 4 : 0;
    emit(cg, "    mov ecx, dword ptr [r10+%d]", field);
    emit(cg, "    mov rax, qword ptr [r10+16]");
    emit(cg, "    add rax, rcx");
    emit(cg, "    add dword ptr [r10+%d], %d", field, is_sse ? 16 : 8);
}

void sysv_va_arg(struct cg* cg, struct node* n)
{
    struct type* t = n->type;
    int cls[2];
    int k = classify(t, cls), ni, ns;
    count_classes(k, cls, &ni, &ns);

    gen_expr(cg, n->lhs);    /* address of the va_list struct */
    emit(cg, "    mov r10, rax");

    int from_stack = new_label(cg), done = new_label(cg);
    if (k > 0)
    {
        /* enough saved registers left? */
        if (ni)
        {
            emit(cg, "    mov ecx, dword ptr [r10]");
            emit(cg, "    cmp ecx, %d", 48 - 8 * ni);
            emit(cg, "    ja .L%d", from_stack);
        }
        if (ns)
        {
            emit(cg, "    mov ecx, dword ptr [r10+4]");
            emit(cg, "    cmp ecx, %d", 176 - 16 * ns);
            emit(cg, "    ja .L%d", from_stack);
        }

        if (!type_is_record(t))
            next_saved_register(cg, cls[0] == C_SSE);
        else
        {
            /* the pieces are not contiguous in the save area: gather them */
            int tmp = alloc_temp(cg, 16, 8);
            for (int j = 0; j < k; j++)
            {
                next_saved_register(cg, cls[j] == C_SSE);
                emit(cg, "    mov r11, qword ptr [rax]");
                emit(cg, "    mov qword ptr [rbp%+d], r11", tmp + 8 * j);
            }
            emit(cg, "    lea rax, [rbp%+d]", tmp);
        }
        emit(cg, "    jmp .L%d", done);
    }

    /* from the stack arguments */
    emit(cg, ".L%d:", from_stack);
    emit(cg, "    mov rax, qword ptr [r10+8]");
    emit(cg, "    lea rcx, [rax+%d]", align_to(t->size, 8));
    emit(cg, "    mov qword ptr [r10+8], rcx");

    emit(cg, ".L%d:", done);
    load(cg, t);
}
