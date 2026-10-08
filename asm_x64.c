/*
 * Assembler, x86-64 instructions: the text written by backend_x64.c
 * -> machine code. Lines, labels and directives are in asm.c.
 *
 * x64 encoding, the parts used here:
 *
 *   [prefix] [REX] opcode [ModRM] [SIB] [displacement] [immediate]
 *
 *   prefix  66 = 16-bit operand; F2/F3/66 also select SSE instructions
 *   REX     0100WRXB  W = 64-bit operand, R/X/B = 4th bit of the
 *           register numbers (r8-r15, xmm8-xmm15)
 *   ModRM   mod(2) reg(3) rm(3)
 *           mod 11 = rm is a register
 *           mod 00/01/10 = memory [rm], [rm+disp8], [rm+disp32]
 *           mod 00 rm 101 = [rip+disp32]
 *           rm 100 = a SIB byte follows (needed when the base is rsp)
 */
#include "asm.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

enum operand_kind { O_NONE, O_REG, O_XMM, O_MEM, O_IMM, O_SYM };

#define BASE_RIP (-1)

struct operand
{
    enum operand_kind kind;
    int reg;                /* O_REG, O_XMM: 0-15 */
    int size;               /* O_REG and O_MEM (from "byte ptr" ...): 1 2 4 8 */
    int base;               /* O_MEM: register number or BASE_RIP */
    long long disp;         /* O_MEM displacement, or addend with rip */
    const char* sym;        /* O_MEM with rip, O_SYM */
    int got;                /* [rip+sym@GOT]: the GOT entry of sym */
    long long imm;          /* O_IMM */
};

/* ----------------------------------------------------------- operands */

static int parse_reg(const char* s, int* num, int* size)
{
    static const char* r64[] = { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi" };
    static const char* r32[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
    static const char* r16[] = { "ax", "cx", "dx", "bx", "sp", "bp", "si", "di" };
    static const char* r8[] = { "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil" };

    for (int i = 0; i < 8; i++)
    {
        if (!strcmp(s, r64[i])) { *num = i; *size = 8; return 1; }
        if (!strcmp(s, r32[i])) { *num = i; *size = 4; return 1; }
        if (!strcmp(s, r16[i])) { *num = i; *size = 2; return 1; }
        if (!strcmp(s, r8[i])) { *num = i; *size = 1; return 1; }
    }
    /* r8 .. r15 with suffix d (32), w (16), b (8) */
    if (s[0] == 'r' && isdigit((unsigned char)s[1]))
    {
        char* end;
        long n = strtol(s + 1, &end, 10);
        if (n < 8 || n > 15) return 0;
        *num = (int)n;
        if (*end == 0) *size = 8;
        else if (!strcmp(end, "d")) *size = 4;
        else if (!strcmp(end, "w")) *size = 2;
        else if (!strcmp(end, "b")) *size = 1;
        else return 0;
        return 1;
    }
    return 0;
}

static void parse_operand(struct assembler* a, char* s, struct operand* op, const char* line)
{
    memset(op, 0, sizeof *op);
    while (*s == ' ') s++;
    int len = (int)strlen(s);
    while (len > 0 && s[len - 1] == ' ') s[--len] = 0;

    /* size: "byte ptr [..]" */
    static const char* sizes[] = { "byte", "word", "dword", "qword" };
    for (int i = 0; i < 4; i++)
    {
        int n = (int)strlen(sizes[i]);
        if (strncmp(s, sizes[i], n) == 0 && s[n] == ' ')
        {
            op->size = 1 << i;
            s += n;
            while (*s == ' ') s++;
            if (strncmp(s, "ptr", 3) == 0) s += 3;
            while (*s == ' ') s++;
        }
    }

    if (*s == '[')
    {
        op->kind = O_MEM;
        char inner[256];
        snprintf(inner, sizeof inner, "%s", s + 1);
        char* close = strchr(inner, ']');
        if (close == NULL) asm_error(a, "missing ]", line);
        *close = 0;

        if (strncmp(inner, "rip+", 4) == 0)
        {
            op->base = BASE_RIP;
            char* at = strstr(inner, "@GOT");
            if (at)
            {
                memmove(at, at + 4, strlen(at + 4) + 1);
                op->got = 1;
            }
            op->sym = parse_sym_addend(inner + 4, &op->disp);
            return;
        }
        char* sign = inner;
        while (*sign && *sign != '+' && *sign != '-') sign++;
        char saved = *sign;
        *sign = 0;
        int size;
        if (!parse_reg(inner, &op->base, &size) || size != 8)
            asm_error(a, "invalid base register", line);
        *sign = saved;
        op->disp = saved ? parse_number(sign) : 0;
        return;
    }

    if (strncmp(s, "xmm", 3) == 0 && isdigit((unsigned char)s[3]))
    {
        op->kind = O_XMM;
        op->reg = atoi(s + 3);
        op->size = 16;
        return;
    }

    if (parse_reg(s, &op->reg, &op->size))
    {
        op->kind = O_REG;
        return;
    }

    if (isdigit((unsigned char)*s) || *s == '-')
    {
        op->kind = O_IMM;
        op->imm = parse_number(s);
        return;
    }

    op->kind = O_SYM;
    op->sym = parse_sym_addend(s, &op->disp);
}

/* ------------------------------------------------------------ encoding */

/*
 * Emits  [prefix] [REX] opcode ModRM [SIB] [disp] [imm].
 * reg is the value of the ModRM reg field (a register or an opcode
 * extension "/digit"); rm is a register or memory operand.
 */
static void encode(struct assembler* a, int prefix, int w, const unsigned char* opcode, int n,
                   int reg, struct operand* rm, long long imm, int imm_size)
{
    int rex = 0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0);
    if (rm->kind == O_REG || rm->kind == O_XMM)
        rex |= (rm->reg & 8) ? 1 : 0;
    else if (rm->base != BASE_RIP)
        rex |= (rm->base & 8) ? 1 : 0;

    if (prefix) out8(a, prefix);
    if (rex != 0x40) out8(a, rex);
    for (int i = 0; i < n; i++) out8(a, opcode[i]);

    if (rm->kind == O_REG || rm->kind == O_XMM)
    {
        out8(a, 0xC0 | (reg & 7) << 3 | (rm->reg & 7));
    }
    else if (rm->base == BASE_RIP)
    {
        /* [rip+disp32]: the linker writes the distance to the symbol */
        out8(a, 0x05 | (reg & 7) << 3);
        int at = here(a);
        add_fixup(a, rm->got ? FIX_GOTREL32 : FIX_REL32, at, at + 4 + imm_size, rm->sym, rm->disp);
        out_n(a, 0, 4);
    }
    else
    {
        int base = rm->base & 7;
        int mod;
        if (rm->disp == 0 && base != 5) mod = 0;          /* rbp/r13 always need a displacement */
        else if (rm->disp >= -128 && rm->disp <= 127) mod = 1;
        else mod = 2;
        out8(a, mod << 6 | (reg & 7) << 3 | base);
        if (base == 4) out8(a, 0x24);                     /* SIB: base rsp/r12, no index */
        if (mod == 1) out8(a, (int)rm->disp & 0xFF);
        if (mod == 2) out_n(a, (unsigned long long)rm->disp, 4);
    }
    out_n(a, (unsigned long long)imm, imm_size);
}

#define OP(...) (const unsigned char[]){ __VA_ARGS__ }, sizeof((const unsigned char[]){ __VA_ARGS__ })

static int fits8(long long v) { return v >= -128 && v <= 127; }
static int fits32(long long v) { return v >= -2147483648LL && v <= 2147483647LL; }

static int is_rm(struct operand* op) { return op->kind == O_REG || op->kind == O_MEM; }

/* jmp/jcc/call to a label: opcode + rel32 */
static void branch(struct assembler* a, const unsigned char* opcode, int n, struct operand* target)
{
    for (int i = 0; i < n; i++) out8(a, opcode[i]);
    int at = here(a);
    add_fixup(a, FIX_REL32, at, at + 4, target->sym, target->disp);
    out_n(a, 0, 4);
}

static int alu_digit(const char* m)
{
    static const char* names[] = { "add", "or", "adc", "sbb", "and", "sub", "xor", "cmp" };
    for (int i = 0; i < 8; i++)
        if (!strcmp(m, names[i])) return i;
    return -1;
}

static int setcc_code(const char* m)
{
    static const struct { const char* name; int code; } cc[] = {
        { "sete", 0x94 }, { "setne", 0x95 }, { "setl", 0x9C }, { "setle", 0x9E },
        { "setb", 0x92 }, { "setbe", 0x96 }, { "seta", 0x97 }, { "setae", 0x93 },
        { "setp", 0x9A }, { "setnp", 0x9B }
    };
    for (int i = 0; i < 10; i++)
        if (!strcmp(m, cc[i].name)) return cc[i].code;
    return -1;
}

/* SSE "xmm, xmm/mem" arithmetic: prefix 0F op */
static int sse_arith(const char* m, int* prefix, int* op)
{
    static const struct { const char* name; int prefix; int op; } t[] = {
        { "addss", 0xF3, 0x58 }, { "addsd", 0xF2, 0x58 }, { "subss", 0xF3, 0x5C }, { "subsd", 0xF2, 0x5C },
        { "mulss", 0xF3, 0x59 }, { "mulsd", 0xF2, 0x59 }, { "divss", 0xF3, 0x5E }, { "divsd", 0xF2, 0x5E },
        { "cvtss2sd", 0xF3, 0x5A }, { "cvtsd2ss", 0xF2, 0x5A }, { "ucomiss", 0, 0x2E }, { "ucomisd", 0x66, 0x2E },
        { "xorps", 0, 0x57 }, { "movaps", 0, 0x28 }
    };
    for (int i = 0; i < 14; i++)
        if (!strcmp(m, t[i].name)) { *prefix = t[i].prefix; *op = t[i].op; return 1; }
    return 0;
}

static void instruction(struct assembler* a, const char* m, struct operand* d, struct operand* s, const char* line)
{
    int code, prefix;

    if (!strcmp(m, "ret")) { out8(a, 0xC3); return; }

    /* cmpxchg r/m, r: 0F B0 (8 bits) / 0F B1 */
    if (!strcmp(m, "cmpxchg"))
    {
        int size = s->size;
        unsigned char op[2] = { 0x0F, (unsigned char)(size == 1 ? 0xB0 : 0xB1) };
        encode(a, size == 2 ? 0x66 : 0, size == 8, op, 2, s->reg, d, 0, 0);
        return;
    }
    if (!strcmp(m, "cqo")) { out8(a, 0x48); out8(a, 0x99); return; }
    if (!strcmp(m, "cdq")) { out8(a, 0x99); return; }

    if (!strcmp(m, "push") || !strcmp(m, "pop"))
    {
        if (d->kind == O_IMM && !strcmp(m, "push") && fits8(d->imm))
        {
            out8(a, 0x6A);
            out8(a, (int)d->imm & 0xFF);
            return;
        }
        if (d->kind != O_REG || d->size != 8) asm_error(a, "invalid operand", line);
        if (d->reg & 8) out8(a, 0x41);
        out8(a, (m[1] == 'u' ? 0x50 : 0x58) + (d->reg & 7));
        return;
    }

    if (!strcmp(m, "jmp")) { branch(a, OP(0xE9), d); return; }
    if (!strcmp(m, "je")) { branch(a, OP(0x0F, 0x84), d); return; }
    if (!strcmp(m, "jne")) { branch(a, OP(0x0F, 0x85), d); return; }
    if (!strcmp(m, "js")) { branch(a, OP(0x0F, 0x88), d); return; }
    if (!strcmp(m, "ja")) { branch(a, OP(0x0F, 0x87), d); return; }
    if (!strcmp(m, "hlt")) { out8(a, 0xF4); return; }
    if (!strcmp(m, "call"))
    {
        if (d->kind == O_REG)
            encode(a, 0, 0, OP(0xFF), 2, d, 0, 0);            /* call r/m64: FF /2 */
        else
            branch(a, OP(0xE8), d);
        return;
    }

    if (!strcmp(m, "mov"))
    {
        int size = d->size ? d->size : s->size;
        int p66 = size == 2 ? 0x66 : 0;
        if (s->kind == O_REG && is_rm(d))
        {
            unsigned char op = size == 1 ? 0x88 : 0x89;      /* mov r/m, r */
            encode(a, p66, size == 8, &op, 1, s->reg, d, 0, 0);
            return;
        }
        if (d->kind == O_REG && s->kind == O_MEM)
        {
            unsigned char op = size == 1 ? 0x8A : 0x8B;      /* mov r, r/m */
            encode(a, p66, size == 8, &op, 1, d->reg, s, 0, 0);
            return;
        }
        if (d->kind == O_REG && s->kind == O_IMM)
        {
            if (size == 8 && fits32(s->imm))
            {
                encode(a, 0, 1, OP(0xC7), 0, d, s->imm, 4);   /* sign-extended imm32 */
                return;
            }
            if (d->reg & 8) out8(a, size == 8 ? 0x49 : 0x41);
            else if (size == 8) out8(a, 0x48);
            out8(a, 0xB8 + (d->reg & 7));
            out_n(a, (unsigned long long)s->imm, size == 8 ? 8 : 4);
            return;
        }
        if (d->kind == O_MEM && s->kind == O_IMM)
        {
            if (size == 1) encode(a, 0, 0, OP(0xC6), 0, d, s->imm, 1);
            else if (size == 2) encode(a, 0x66, 0, OP(0xC7), 0, d, s->imm, 2);
            else encode(a, 0, size == 8, OP(0xC7), 0, d, s->imm, 4);
            return;
        }
        asm_error(a, "invalid mov", line);
    }

    if (!strcmp(m, "movabs"))
    {
        out8(a, 0x48 | ((d->reg & 8) ? 1 : 0));
        out8(a, 0xB8 + (d->reg & 7));
        out_n(a, (unsigned long long)s->imm, 8);
        return;
    }

    if (!strcmp(m, "movzx") || !strcmp(m, "movsx"))
    {
        int sz = s->size;
        int op = (m[3] == 'z' ? 0xB6 : 0xBE) + (sz == 2 ? 1 : 0);
        encode(a, 0, d->size == 8, OP(0x0F, (unsigned char)op), d->reg, s, 0, 0);
        return;
    }
    if (!strcmp(m, "movsxd")) { encode(a, 0, 1, OP(0x63), d->reg, s, 0, 0); return; }
    if (!strcmp(m, "lea")) { encode(a, 0, 1, OP(0x8D), d->reg, s, 0, 0); return; }

    if ((code = alu_digit(m)) >= 0)
    {
        int size = d->size ? d->size : s->size;
        int p66 = size == 2 ? 0x66 : 0;
        int w = size == 8;
        if (s->kind == O_IMM)
        {
            if (size == 1) encode(a, 0, 0, OP(0x80), code, d, s->imm, 1);
            else if (fits8(s->imm)) encode(a, p66, w, OP(0x83), code, d, s->imm, 1);
            else encode(a, p66, w, OP(0x81), code, d, s->imm, size == 2 ? 2 : 4);
            return;
        }
        if (s->kind == O_REG)
        {
            unsigned char op = (unsigned char)(code * 8 + (size == 1 ? 0 : 1));   /* op r/m, r */
            encode(a, p66, w, &op, 1, s->reg, d, 0, 0);
            return;
        }
        if (d->kind == O_REG && s->kind == O_MEM)
        {
            unsigned char op = (unsigned char)(code * 8 + (size == 1 ? 2 : 3));   /* op r, r/m */
            encode(a, p66, w, &op, 1, d->reg, s, 0, 0);
            return;
        }
        asm_error(a, "invalid operands", line);
    }

    if (!strcmp(m, "test"))
    {
        unsigned char op = d->size == 1 ? 0x84 : 0x85;
        encode(a, 0, d->size == 8, &op, 1, s->reg, d, 0, 0);
        return;
    }
    if (!strcmp(m, "imul")) { encode(a, 0, d->size == 8, OP(0x0F, 0xAF), d->reg, s, 0, 0); return; }

    /* F7 group: not /2, neg /3, div /6, idiv /7 */
    if (!strcmp(m, "not")) { encode(a, 0, d->size == 8, OP(0xF7), 2, d, 0, 0); return; }
    if (!strcmp(m, "neg")) { encode(a, 0, d->size == 8, OP(0xF7), 3, d, 0, 0); return; }
    if (!strcmp(m, "div")) { encode(a, 0, d->size == 8, OP(0xF7), 6, d, 0, 0); return; }
    if (!strcmp(m, "idiv")) { encode(a, 0, d->size == 8, OP(0xF7), 7, d, 0, 0); return; }

    /* shifts: shl /4, shr /5, sar /7; by cl (D3) or by imm8 (C1) */
    if (!strcmp(m, "shl") || !strcmp(m, "shr") || !strcmp(m, "sar"))
    {
        int digit = m[2] == 'l' ? 4 : m[1] == 'h' ? 5 : 7;
        if (s->kind == O_REG) encode(a, 0, d->size == 8, OP(0xD3), digit, d, 0, 0);
        else encode(a, 0, d->size == 8, OP(0xC1), digit, d, s->imm, 1);
        return;
    }
    if (!strcmp(m, "btc")) { encode(a, 0, 1, OP(0x0F, 0xBA), 7, d, s->imm, 1); return; }

    if ((code = setcc_code(m)) >= 0)
    {
        unsigned char op[2] = { 0x0F, (unsigned char)code };
        encode(a, 0, 0, op, 2, 0, d, 0, 0);
        return;
    }

    /* ----- SSE ----- */
    if (!strcmp(m, "movss") || !strcmp(m, "movsd"))
    {
        prefix = m[4] == 's' ? 0xF3 : 0xF2;
        if (d->kind == O_XMM) encode(a, prefix, 0, OP(0x0F, 0x10), d->reg, s, 0, 0);
        else encode(a, prefix, 0, OP(0x0F, 0x11), s->reg, d, 0, 0);
        return;
    }
    if (!strcmp(m, "movq"))
    {
        if (d->kind == O_XMM && s->kind == O_REG) encode(a, 0x66, 1, OP(0x0F, 0x6E), d->reg, s, 0, 0);
        else if (d->kind == O_REG && s->kind == O_XMM) encode(a, 0x66, 1, OP(0x0F, 0x7E), s->reg, d, 0, 0);
        else if (d->kind == O_XMM) encode(a, 0xF3, 0, OP(0x0F, 0x7E), d->reg, s, 0, 0);
        else encode(a, 0x66, 0, OP(0x0F, 0xD6), s->reg, d, 0, 0);
        return;
    }
    if (!strcmp(m, "movd"))
    {
        if (d->kind == O_XMM) encode(a, 0x66, 0, OP(0x0F, 0x6E), d->reg, s, 0, 0);
        else encode(a, 0x66, 0, OP(0x0F, 0x7E), s->reg, d, 0, 0);
        return;
    }
    if (!strcmp(m, "cvtsi2sd") || !strcmp(m, "cvtsi2ss"))
    {
        encode(a, m[7] == 'd' ? 0xF2 : 0xF3, s->size == 8, OP(0x0F, 0x2A), d->reg, s, 0, 0);
        return;
    }
    if (!strcmp(m, "cvttsd2si") || !strcmp(m, "cvttss2si"))
    {
        encode(a, m[5] == 'd' ? 0xF2 : 0xF3, d->size == 8, OP(0x0F, 0x2C), d->reg, s, 0, 0);
        return;
    }
    if (sse_arith(m, &prefix, &code))
    {
        unsigned char op[2] = { 0x0F, (unsigned char)code };
        encode(a, prefix, 0, op, 2, d->reg, s, 0, 0);
        return;
    }

    asm_error(a, "unknown instruction", line);
}

/* ------------------------------------------------------------- driver */

void x64_line(struct assembler* a, char* s, const char* line)
{
    /* lock prefix (F0): the next instruction is atomic */
    if (strncmp(s, "lock ", 5) == 0)
    {
        out8(a, 0xF0);
        s += 5;
        while (*s == ' ') s++;
    }

    /* mnemonic operand, operand */
    char* ops = s;
    while (*ops && *ops != ' ') ops++;
    if (*ops) *ops++ = 0;

    struct operand d = { 0 }, src = { 0 };
    char* comma = strchr(ops, ',');
    if (comma)
    {
        *comma = 0;
        parse_operand(a, comma + 1, &src, s);
    }
    if (*ops)
        parse_operand(a, ops, &d, s);
    instruction(a, s, &d, &src, line);
}
