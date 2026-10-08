#ifndef ASM_H
#define ASM_H

/*
 * Assembler, the part shared by every instruction set: lines, labels,
 * directives, sections, symbols and fixups (asm.c). Each instruction
 * set encodes its own instructions:
 *     asm_x64.c     x86-64
 *     asm_arm64.c   AArch64
 */
#include "object.h"
#include "cc89.h"

struct assembler
{
    struct object* obj;
    int sec;
    int line;
    int selectany_next;     /* .selectany seen: applies to the next label */
};

void asm_error(struct assembler* a, const char* msg, const char* line);
void out8(struct assembler* a, int b);
void out_n(struct assembler* a, unsigned long long v, int n);
int here(struct assembler* a);
void add_fixup(struct assembler* a, enum fixup_kind kind, int offset, int end, const char* name, long long addend);

long long parse_number(const char* s);
int is_sym_char(char c);
const char* parse_sym_addend(const char* s, long long* addend);

/* one instruction line (mnemonic and operands, no label, no comment) */
void x64_line(struct assembler* a, char* s, const char* line);
void arm64_line(struct assembler* a, char* s, const char* line);

#endif
