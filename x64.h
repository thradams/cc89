#ifndef X64_H
#define X64_H

/*
 * Internal header of the x64 back-end.
 *
 *   backend_x64.c   the stack machine: expressions, statements, data
 *   abi_win64.c     Microsoft x64 calling convention (Windows)
 *   abi_sysv.c      System V AMD64 calling convention (Linux)
 *
 * The ABI files only decide where arguments, parameters, return values
 * and variable arguments live. Everything else is shared.
 */
#include "backend.h"
#include "dwarf.h"
#include "codeview.h"

struct cg
{
    struct text* out;   /* the whole program */
    struct text body;   /* body of the current function, written before its prologue */
    struct obj* fn;
    int sysv;           /* 1 = System V ABI, 0 = Microsoft ABI */
    int depth;          /* 8-byte values pushed on the stack right now */
    int frame_size;     /* bytes used by locals and temporaries */
    int label_count;
    int break_label;
    int continue_label;
    int return_label;

    /* filled by the ABI when the function starts */
    int hidden_ret;         /* returns a struct through a hidden pointer */
    int ret_ptr_offset;     /* System V: where the hidden pointer was saved */
    int reg_save_offset;    /* System V variadic: register save area */
    int va_gp_offset;       /* System V variadic: first free gp register (bytes) */
    int va_fp_offset;       /* System V variadic: first free xmm register (bytes) */
    int va_overflow_offset; /* System V variadic: first stack argument (rbp+n) */
    struct text prologue;   /* ABI part of the prologue: saves the argument registers */
    struct dwarf* dw;       /* -g on Linux: DWARF, else NULL */
    struct codeview* cv;    /* -g on Windows: CodeView, else NULL */
};

/* ------------------------------------------------- shared helpers */

void emit(struct cg* cg, const char* fmt, ...);
int new_label(struct cg* cg);
int align_to(int n, int align);
int alloc_temp(struct cg* cg, int size, int align);
int is_flt(struct type* t);
const char* sfx(struct type* t);

void push(struct cg* cg, const char* reg);
void pop(struct cg* cg, const char* reg);
void push_xmm(struct cg* cg, int r);
void pop_xmm(struct cg* cg, int r);
void push_value(struct cg* cg, struct type* t);

void normalize(struct cg* cg, struct type* t);
void load(struct cg* cg, struct type* t);
void store(struct cg* cg, struct type* t);
void copy_bytes(struct cg* cg, int size);

void gen_expr(struct cg* cg, struct node* n);

/* ------------------------------------------------- ABI interface */

/* Microsoft x64 */
int win64_by_reference(struct type* t);
void win64_params(struct cg* cg, struct obj* fn);   /* before the body: offsets */
void win64_prologue(struct cg* cg, struct obj* fn); /* after the body: cg->prologue */
void win64_call(struct cg* cg, struct node* n);
void win64_return(struct cg* cg, struct type* t);
void win64_va_start(struct cg* cg, struct node* n);
void win64_va_arg(struct cg* cg, struct node* n);

/* System V AMD64 */
void sysv_params(struct cg* cg, struct obj* fn);
void sysv_prologue(struct cg* cg, struct obj* fn);
void sysv_call(struct cg* cg, struct node* n);
void sysv_return(struct cg* cg, struct type* t);
void sysv_va_start(struct cg* cg, struct node* n);
void sysv_va_arg(struct cg* cg, struct node* n);

#endif
