/*
 * Assembler, AArch64 instructions: the text written by backend_arm64.c
 * -> machine code. Lines, labels and directives are in asm.c.
 *
 * Every instruction is 32 bits. The fields used here:
 *
 *   sf (bit 31)      1 = 64-bit registers (x0..), 0 = 32-bit (w0..)
 *   Rd, Rn, Rm       register numbers, 5 bits each (bits 0, 5, 16)
 *   register 31      sp in some instructions, xzr/wzr (zero) in others
 *   imm12            unsigned 12-bit immediate (add, sub, ldr offsets)
 *   imm9             signed 9-bit offset (pre/post-index, unscaled)
 *
 * Addresses of symbols take two instructions:
 *     adrp x0, sym              x0 = address of the 4096-byte page of sym
 *     add  x0, x0, :lo12:sym    + the offset of sym inside the page
 * and the linker fills both (fixups FIX_A64_PAGE21 and FIX_A64_LO12).
 * For a symbol of a shared library the address is in the GOT:
 *     adrp x0, :got:sym
 *     ldr  x0, [x0, :got_lo12:sym]
 */
#include "asm.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

enum a64_kind { A_NONE, A_REG, A_FP, A_IMM, A_MEM, A_SYM };

enum a64_reloc { REL_NONE, REL_LO12, REL_GOT, REL_GOT_LO12 };

enum mem_mode { M_OFFSET, M_PRE, M_POST };

struct a64op
{
    enum a64_kind kind;
    int reg;                /* A_REG, A_FP, A_MEM base: 0..31 */
    int is64;               /* A_REG: x (1) or w (0) */
    int is_sp;              /* A_REG: sp / wsp (number 31) */
    int fpsize;             /* A_FP: 4 (s), 8 (d), 16 (q) */
    long long imm;          /* A_IMM, A_MEM offset */
    int shift;              /* A_IMM: "lsl #n" after it */
    enum mem_mode mode;
    const char* sym;        /* A_SYM, or A_MEM with :lo12: offset */
    long long addend;
    enum a64_reloc reloc;
};

static void out32(struct assembler* a, unsigned v)
{
    out_n(a, v, 4);
}

/* -------------------------------------------------------------- operands */

static int parse_gp(const char* s, struct a64op* op)
{
    if (!strcmp(s, "sp")) { op->reg = 31; op->is64 = 1; op->is_sp = 1; return 1; }
    if (!strcmp(s, "wsp")) { op->reg = 31; op->is64 = 0; op->is_sp = 1; return 1; }
    if (!strcmp(s, "xzr")) { op->reg = 31; op->is64 = 1; return 1; }
    if (!strcmp(s, "wzr")) { op->reg = 31; op->is64 = 0; return 1; }
    if (!strcmp(s, "fp")) { op->reg = 29; op->is64 = 1; return 1; }
    if (!strcmp(s, "lr")) { op->reg = 30; op->is64 = 1; return 1; }
    if ((s[0] == 'x' || s[0] == 'w') && isdigit((unsigned char)s[1]))
    {
        char* end;
        long n = strtol(s + 1, &end, 10);
        if (*end || n > 30) return 0;
        op->reg = (int)n;
        op->is64 = s[0] == 'x';
        return 1;
    }
    return 0;
}

static int parse_fp(const char* s, struct a64op* op)
{
    if ((s[0] == 's' || s[0] == 'd' || s[0] == 'q') && isdigit((unsigned char)s[1]))
    {
        char* end;
        long n = strtol(s + 1, &end, 10);
        if (*end || n > 31) return 0;
        op->reg = (int)n;
        op->fpsize = s[0] == 's' ? 4 : s[0] == 'd' ? 8 : 16;
        return 1;
    }
    return 0;
}

/* ":lo12:sym+4" -> reloc, symbol, addend */
static void parse_reloc_sym(const char* s, struct a64op* op)
{
    op->reloc = REL_NONE;
    if (!strncmp(s, ":lo12:", 6)) { op->reloc = REL_LO12; s += 6; }
    else if (!strncmp(s, ":got_lo12:", 10)) { op->reloc = REL_GOT_LO12; s += 10; }
    else if (!strncmp(s, ":got:", 5)) { op->reloc = REL_GOT; s += 5; }
    op->sym = parse_sym_addend(s, &op->addend);
}

static char* trim(char* s)
{
    while (*s == ' ') s++;
    int n = (int)strlen(s);
    while (n > 0 && s[n - 1] == ' ') s[--n] = 0;
    return s;
}

static void parse_operand(struct assembler* a, char* s, struct a64op* op, const char* line)
{
    memset(op, 0, sizeof *op);
    s = trim(s);

    if (*s == '[')
    {
        /* [xN]  [xN, #off]  [xN, #off]!  [xN, :lo12:sym] */
        op->kind = A_MEM;
        char* close = strchr(s, ']');
        if (close == NULL) asm_error(a, "missing ]", line);
        op->mode = close[1] == '!' ? M_PRE : M_OFFSET;
        *close = 0;
        char* comma = strchr(s + 1, ',');
        if (comma) *comma = 0;
        struct a64op base = { 0 };
        if (!parse_gp(trim(s + 1), &base) || !base.is64)
            asm_error(a, "invalid base register", line);
        op->reg = base.reg;
        if (comma)
        {
            char* off = trim(comma + 1);
            if (*off == '#') op->imm = parse_number(off + 1);
            else if (*off == ':') parse_reloc_sym(off, op);
            else asm_error(a, "invalid offset", line);
        }
        return;
    }
    if (*s == '#')
    {
        op->kind = A_IMM;
        op->imm = parse_number(s + 1);
        return;
    }
    if (parse_gp(s, op)) { op->kind = A_REG; return; }
    if (parse_fp(s, op)) { op->kind = A_FP; return; }
    op->kind = A_SYM;
    parse_reloc_sym(s, op);
}

/* split at the commas outside [ ] */
static int split_operands(char* s, char** parts, int max)
{
    int n = 0, depth = 0;
    if (*trim(s) == 0) return 0;
    parts[n++] = s;
    for (char* c = s; *c; c++)
    {
        if (*c == '[') depth++;
        else if (*c == ']') depth--;
        else if (*c == ',' && depth == 0 && n < max)
        {
            *c = 0;
            parts[n++] = c + 1;
        }
    }
    return n;
}

static int cond_code(const char* s)
{
    static const char* names[] = { "eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc",
                                   "hi", "ls", "ge", "lt", "gt", "le", "al" };
    for (int i = 0; i < 15; i++)
        if (!strcmp(s, names[i])) return i;
    if (!strcmp(s, "cs")) return 2;
    if (!strcmp(s, "cc")) return 3;
    return -1;
}

/* ------------------------------------------------------------- encoding */

#define SF(op) ((unsigned)(op)->is64 << 31)

static unsigned rd(struct a64op* op) { return (unsigned)op->reg & 31; }

/* label as target of a branch: the linker fills the offset */
static void branch_fixup(struct assembler* a, enum fixup_kind kind, struct a64op* target)
{
    add_fixup(a, kind, here(a), 0, target->sym, target->addend);
}

/* ldr/str with an address in [Xn ...]; size in bytes, fp = vector register */
static void load_store(struct assembler* a, int is_load, int sign_extend_to64, int size, int fp,
                       struct a64op* rt, struct a64op* mem, struct a64op* post, const char* line)
{
    unsigned sizebits, opc, v = fp ? 1u : 0u;
    switch (size)
    {
        case 1: sizebits = 0; break;
        case 2: sizebits = 1; break;
        case 4: sizebits = 2; break;
        case 8: sizebits = 3; break;
        default: sizebits = 0; break;   /* 16: q register */
    }
    if (fp && size == 16)
        opc = is_load ? 3 : 2;
    else if (!is_load)
        opc = 0;
    else if (sign_extend_to64)
        opc = size == 4 ? 2 : 2;        /* ldrsb/ldrsh/ldrsw to x */
    else
        opc = 1;
    unsigned base = (unsigned)mem->reg & 31, t = rd(rt);

    if (post || mem->mode == M_PRE)
    {
        long long off = post ? post->imm : mem->imm;
        if (off < -256 || off > 255) asm_error(a, "offset out of range", line);
        out32(a, 0x38000000u | sizebits << 30 | v << 26 | opc << 22 | ((unsigned)off & 0x1FF) << 12 |
                 (post ? 1u : 3u) << 10 | base << 5 | t);
        return;
    }

    if (mem->reloc == REL_LO12 || mem->reloc == REL_GOT_LO12)
    {
        /* ldr x0, [x0, :got_lo12:sym]: scaled offset filled by the linker */
        add_fixup(a, mem->reloc == REL_GOT_LO12 ? FIX_A64_GOT_LO12 : FIX_A64_LO12, here(a), 0, mem->sym, mem->addend);
        out32(a, 0x39000000u | sizebits << 30 | v << 26 | opc << 22 | base << 5 | t);
        return;
    }

    long long off = mem->imm;
    int scale = size;
    if (off >= 0 && off % scale == 0 && off / scale < 4096)
    {
        /* unsigned offset, scaled by the size */
        out32(a, 0x39000000u | sizebits << 30 | v << 26 | opc << 22 | (unsigned)(off / scale) << 10 | base << 5 | t);
        return;
    }
    if (off >= -256 && off <= 255)
    {
        /* unscaled signed offset (ldur/stur) */
        out32(a, 0x38000000u | sizebits << 30 | v << 26 | opc << 22 | ((unsigned)off & 0x1FF) << 12 | base << 5 | t);
        return;
    }
    asm_error(a, "offset out of range", line);
}

/* sbfm / ubfm / bfm */
static void bitfield_move(struct assembler* a, unsigned opc_base, struct a64op* d, struct a64op* n, int immr, int imms)
{
    unsigned sf = (unsigned)d->is64;
    out32(a, opc_base | sf << 31 | sf << 22 | (unsigned)immr << 16 | (unsigned)imms << 10 | rd(n) << 5 | rd(d));
}

static void instruction(struct assembler* a, const char* m, struct a64op* op, int n, const char* line)
{
    struct a64op* d = &op[0];

    /* ----- branches ----- */
    if (!strcmp(m, "b") || !strcmp(m, "bl"))
    {
        branch_fixup(a, FIX_A64_CALL26, d);
        out32(a, m[1] == 'l' ? 0x94000000u : 0x14000000u);
        return;
    }
    if (!strncmp(m, "b.", 2))
    {
        int c = cond_code(m + 2);
        if (c < 0) asm_error(a, "invalid condition", line);
        branch_fixup(a, FIX_A64_COND19, d);
        out32(a, 0x54000000u | (unsigned)c);
        return;
    }
    if (!strcmp(m, "cbz") || !strcmp(m, "cbnz"))
    {
        branch_fixup(a, FIX_A64_COND19, &op[1]);
        out32(a, (m[2] == 'n' ? 0x35000000u : 0x34000000u) | SF(d) | rd(d));
        return;
    }
    if (!strcmp(m, "br")) { out32(a, 0xD61F0000u | rd(d) << 5); return; }
    if (!strcmp(m, "blr")) { out32(a, 0xD63F0000u | rd(d) << 5); return; }
    if (!strcmp(m, "ret")) { out32(a, 0xD65F03C0u); return; }
    if (!strcmp(m, "brk")) { out32(a, 0xD4200000u | ((unsigned)d->imm & 0xFFFF) << 5); return; }
    if (!strcmp(m, "nop")) { out32(a, 0xD503201Fu); return; }

    /* ----- addresses ----- */
    if (!strcmp(m, "adrp"))
    {
        struct a64op* s = &op[1];
        add_fixup(a, s->reloc == REL_GOT ? FIX_A64_GOT_PAGE21 : FIX_A64_PAGE21, here(a), 0, s->sym, s->addend);
        out32(a, 0x90000000u | rd(d));
        return;
    }

    /* ----- moves ----- */
    if (!strcmp(m, "movz") || !strcmp(m, "movk") || !strcmp(m, "movn"))
    {
        unsigned base = m[3] == 'z' ? 0x52800000u : m[3] == 'k' ? 0x72800000u : 0x12800000u;
        unsigned hw = (unsigned)(n > 2 ? op[2].imm : 0) / 16;
        out32(a, base | SF(d) | hw << 21 | ((unsigned)op[1].imm & 0xFFFF) << 5 | rd(d));
        return;
    }
    if (!strcmp(m, "mov"))
    {
        struct a64op* s = &op[1];
        if (d->kind == A_REG && s->kind == A_REG)
        {
            if (d->is_sp || s->is_sp)                   /* add d, s, #0 */
                out32(a, 0x11000000u | SF(d) | rd(s) << 5 | rd(d));
            else                                        /* orr d, zr, s */
                out32(a, 0x2A0003E0u | SF(d) | rd(s) << 16 | rd(d));
            return;
        }
        asm_error(a, "invalid mov", line);
    }
    if (!strcmp(m, "fmov"))
    {
        struct a64op* s = &op[1];
        if (d->kind == A_FP && s->kind == A_FP)
            out32(a, 0x1E204000u | (d->fpsize == 8 ? 1u : 0u) << 22 | rd(s) << 5 | rd(d));
        else if (d->kind == A_REG && s->kind == A_FP)   /* fmov x0, d0 / w0, s0 */
            out32(a, (s->fpsize == 8 ? 0x9E660000u : 0x1E260000u) | rd(s) << 5 | rd(d));
        else if (d->kind == A_FP && s->kind == A_REG)   /* fmov d0, x0 / s0, w0 */
            out32(a, (d->fpsize == 8 ? 0x9E670000u : 0x1E270000u) | rd(s) << 5 | rd(d));
        else
            asm_error(a, "invalid fmov", line);
        return;
    }

    /* ----- add / sub / cmp ----- */
    if (!strcmp(m, "add") || !strcmp(m, "sub") || !strcmp(m, "adds") || !strcmp(m, "subs") || !strcmp(m, "cmp"))
    {
        int is_sub = m[0] == 's' || m[0] == 'c';
        int set_flags = m[0] == 'c' || m[3] == 's';
        struct a64op zr = { 0 };
        zr.kind = A_REG; zr.reg = 31; zr.is64 = op[0].is64;
        struct a64op* dst = m[0] == 'c' ? &zr : &op[0];
        struct a64op* s1 = m[0] == 'c' ? &op[0] : &op[1];
        struct a64op* s2 = m[0] == 'c' ? &op[1] : &op[2];
        unsigned sf = (unsigned)s1->is64;
        if (s2->kind == A_IMM || (s2->kind == A_SYM && s2->reloc == REL_LO12))
        {
            long long imm = s2->kind == A_IMM ? s2->imm : 0;
            if (s2->kind == A_SYM)
                add_fixup(a, FIX_A64_LO12, here(a), 0, s2->sym, s2->addend);
            if (imm < 0 || imm > 4095) asm_error(a, "immediate out of range", line);
            /* "#n, lsl #12": the immediate is shifted 12 bits (bit 22) */
            unsigned sh = m[0] != 'c' && n > 3 && op[3].kind == A_IMM && op[3].imm == 12 ? 1u : 0u;
            unsigned base = is_sub ? (set_flags ? 0x71000000u : 0x51000000u) : (set_flags ? 0x31000000u : 0x11000000u);
            out32(a, base | sf << 31 | sh << 22 | (unsigned)imm << 10 | rd(s1) << 5 | rd(dst));
            return;
        }
        unsigned base = is_sub ? (set_flags ? 0x6B000000u : 0x4B000000u) : (set_flags ? 0x2B000000u : 0x0B000000u);
        out32(a, base | sf << 31 | rd(s2) << 16 | rd(s1) << 5 | rd(dst));
        return;
    }
    if (!strcmp(m, "neg"))      /* sub d, zr, s */
    {
        out32(a, 0x4B0003E0u | SF(d) | rd(&op[1]) << 16 | rd(d));
        return;
    }

    /* ----- logic, multiply, divide, shifts by register ----- */
    {
        static const struct { const char* name; unsigned code; } three[] = {
            { "and", 0x0A000000u }, { "orr", 0x2A000000u }, { "eor", 0x4A000000u },
            { "sdiv", 0x1AC00C00u }, { "udiv", 0x1AC00800u },
            { "lsl", 0x1AC02000u }, { "lsr", 0x1AC02400u }, { "asr", 0x1AC02800u },
        };
        for (int i = 0; i < 8; i++)
        {
            if (strcmp(m, three[i].name) != 0)
                continue;
            if (op[2].kind == A_IMM)
            {
                /* shifts by a constant are bit-field moves */
                int bits = d->is64 ? 64 : 32, s = (int)op[2].imm;
                if (m[0] == 'l' && m[2] == 'l') bitfield_move(a, 0x53000000u, d, &op[1], (bits - s) % bits, bits - 1 - s);
                else if (m[0] == 'l') bitfield_move(a, 0x53000000u, d, &op[1], s, bits - 1);
                else if (m[0] == 'a') bitfield_move(a, 0x13000000u, d, &op[1], s, bits - 1);
                else asm_error(a, "invalid immediate", line);
                return;
            }
            out32(a, three[i].code | SF(d) | rd(&op[2]) << 16 | rd(&op[1]) << 5 | rd(d));
            return;
        }
    }
    if (!strcmp(m, "mvn"))      /* orn d, zr, s */
    {
        out32(a, 0x2A2003E0u | SF(d) | rd(&op[1]) << 16 | rd(d));
        return;
    }
    if (!strcmp(m, "mul"))      /* madd d, n, m, zr */
    {
        out32(a, 0x1B007C00u | SF(d) | rd(&op[2]) << 16 | rd(&op[1]) << 5 | rd(d));
        return;
    }
    if (!strcmp(m, "msub"))     /* d = a - n * m */
    {
        out32(a, 0x1B008000u | SF(d) | rd(&op[2]) << 16 | rd(&op[3]) << 10 | rd(&op[1]) << 5 | rd(d));
        return;
    }

    /* ----- extensions and bit-fields ----- */
    if (!strcmp(m, "sxtb")) { bitfield_move(a, 0x13000000u, d, &op[1], 0, 7); return; }
    if (!strcmp(m, "sxth")) { bitfield_move(a, 0x13000000u, d, &op[1], 0, 15); return; }
    if (!strcmp(m, "sxtw")) { bitfield_move(a, 0x13000000u, d, &op[1], 0, 31); return; }
    if (!strcmp(m, "uxtb")) { bitfield_move(a, 0x53000000u, d, &op[1], 0, 7); return; }
    if (!strcmp(m, "uxth")) { bitfield_move(a, 0x53000000u, d, &op[1], 0, 15); return; }
    if (!strcmp(m, "ubfx") || !strcmp(m, "sbfx"))
    {
        int lsb = (int)op[2].imm, width = (int)op[3].imm;
        bitfield_move(a, m[0] == 'u' ? 0x53000000u : 0x13000000u, d, &op[1], lsb, lsb + width - 1);
        return;
    }
    if (!strcmp(m, "bfi"))
    {
        int bits = d->is64 ? 64 : 32, lsb = (int)op[2].imm, width = (int)op[3].imm;
        bitfield_move(a, 0x33000000u, d, &op[1], (bits - lsb) % bits, width - 1);
        return;
    }

    /* ----- compare and set ----- */
    if (!strcmp(m, "cset"))     /* csinc d, zr, zr, !cond */
    {
        int c = cond_code(op[1].sym);
        if (c < 0) asm_error(a, "invalid condition", line);
        out32(a, 0x1A9F07E0u | SF(d) | (unsigned)(c ^ 1) << 12 | rd(d));
        return;
    }

    /* ----- loads and stores ----- */
    {
        static const struct { const char* name; int load, sext, size; } ls[] = {
            { "ldrb", 1, 0, 1 }, { "ldrsb", 1, 1, 1 }, { "strb", 0, 0, 1 },
            { "ldrh", 1, 0, 2 }, { "ldrsh", 1, 1, 2 }, { "strh", 0, 0, 2 },
            { "ldrsw", 1, 1, 4 }, { "ldr", 1, 0, 0 }, { "str", 0, 0, 0 },
        };
        for (int i = 0; i < 9; i++)
        {
            if (strcmp(m, ls[i].name) != 0)
                continue;
            int size = ls[i].size;
            int fp = d->kind == A_FP;
            if (size == 0)
                size = fp ? d->fpsize : (d->is64 ? 8 : 4);
            struct a64op* post = n > 2 && op[2].kind == A_IMM ? &op[2] : NULL;
            load_store(a, ls[i].load, ls[i].sext, size, fp, d, &op[1], post, line);
            return;
        }
    }
    if (!strcmp(m, "stp") || !strcmp(m, "ldp"))
    {
        /* only the frame forms: stp x29, x30, [sp, #-16]!  and  ldp x29, x30, [sp], #16 */
        struct a64op* mem = &op[2];
        int post = n > 3;
        long long off = post ? op[3].imm : mem->imm;
        unsigned base = m[0] == 's' ? (post ? 0xA8800000u : 0xA9800000u) : (post ? 0xA8C00000u : 0xA9C00000u);
        if (!post && mem->mode != M_PRE) base = m[0] == 's' ? 0xA9000000u : 0xA9400000u;
        out32(a, base | ((unsigned)(off / 8) & 0x7F) << 15 | rd(&op[1]) << 10 | ((unsigned)mem->reg & 31) << 5 | rd(d));
        return;
    }

    /* ----- floating point ----- */
    {
        static const struct { const char* name; unsigned code; } fp3[] = {
            { "fadd", 0x1E202800u }, { "fsub", 0x1E203800u }, { "fmul", 0x1E200800u }, { "fdiv", 0x1E201800u },
        };
        for (int i = 0; i < 4; i++)
            if (!strcmp(m, fp3[i].name))
            {
                out32(a, fp3[i].code | (d->fpsize == 8 ? 1u : 0u) << 22 | rd(&op[2]) << 16 | rd(&op[1]) << 5 | rd(d));
                return;
            }
    }
    if (!strcmp(m, "fneg"))
    {
        out32(a, 0x1E214000u | (d->fpsize == 8 ? 1u : 0u) << 22 | rd(&op[1]) << 5 | rd(d));
        return;
    }
    if (!strcmp(m, "fcmp"))
    {
        unsigned type = d->fpsize == 8 ? 1u : 0u;
        if (op[1].kind == A_IMM)    /* fcmp d0, #0.0 */
            out32(a, 0x1E202008u | type << 22 | rd(d) << 5);
        else
            out32(a, 0x1E202000u | type << 22 | rd(&op[1]) << 16 | rd(d) << 5);
        return;
    }
    if (!strcmp(m, "fcvt"))
    {
        /* d <- s or s <- d */
        out32(a, (d->fpsize == 8 ? 0x1E22C000u : 0x1E624000u) | rd(&op[1]) << 5 | rd(d));
        return;
    }
    if (!strcmp(m, "scvtf") || !strcmp(m, "ucvtf"))
    {
        struct a64op* s = &op[1];
        out32(a, (m[0] == 's' ? 0x1E220000u : 0x1E230000u) | SF(s) | (d->fpsize == 8 ? 1u : 0u) << 22 | rd(s) << 5 | rd(d));
        return;
    }
    if (!strcmp(m, "fcvtzs") || !strcmp(m, "fcvtzu"))
    {
        struct a64op* s = &op[1];
        out32(a, (m[5] == 's' ? 0x1E380000u : 0x1E390000u) | SF(d) | (s->fpsize == 8 ? 1u : 0u) << 22 | rd(s) << 5 | rd(d));
        return;
    }

    asm_error(a, "unknown instruction", line);
}

void arm64_line(struct assembler* a, char* s, const char* line)
{
    char* ops = s;
    while (*ops && *ops != ' ') ops++;
    if (*ops) *ops++ = 0;

    char* parts[6];
    int n = split_operands(ops, parts, 6);
    struct a64op op[6];
    memset(op, 0, sizeof op);
    for (int i = 0; i < n; i++)
    {
        char* p = trim(parts[i]);
        /* "lsl #16" after a movz/movk immediate */
        if (!strncmp(p, "lsl #", 5) && i > 0 && op[i - 1].kind == A_IMM)
        {
            op[i].kind = A_IMM;
            op[i].imm = parse_number(p + 5);
            continue;
        }
        parse_operand(a, p, &op[i], line);
    }
    if (n < 6 && !strcmp(s, "movz") && n == 2) op[2].imm = 0;
    instruction(a, s, op, n, line);
}

/* ---------------------------------------------------------------- linker */

/*
 * Write the target address S into the instruction at P (used by the ELF
 * and Mach-O linkers). For the GOT kinds S is the address of the entry.
 */
void a64_patch(unsigned char* at, enum fixup_kind kind, unsigned long long S, unsigned long long P)
{
    unsigned ins = (unsigned)at[0] | (unsigned)at[1] << 8 | (unsigned)at[2] << 16 | (unsigned)at[3] << 24;
    long long delta = (long long)(S - P);
    switch (kind)
    {
        case FIX_A64_CALL26:
            ins |= (unsigned)(delta >> 2) & 0x3FFFFFF;
            break;
        case FIX_A64_COND19:
            ins |= ((unsigned)(delta >> 2) & 0x7FFFF) << 5;
            break;
        case FIX_A64_PAGE21:
        case FIX_A64_GOT_PAGE21:
        {
            long long pages = (long long)((S & ~0xFFFull) - (P & ~0xFFFull)) >> 12;
            ins |= ((unsigned)pages & 3) << 29;                 /* immlo */
            ins |= (((unsigned)pages >> 2) & 0x7FFFF) << 5;     /* immhi */
            break;
        }
        case FIX_A64_LO12:
        {
            /* add: the 12 bits as they are; ldr/str: divided by the access size */
            unsigned lo = (unsigned)(S & 0xFFF);
            if ((ins & 0x3B000000u) == 0x39000000u)             /* load/store unsigned offset */
                lo >>= ins >> 30;
            ins |= lo << 10;
            break;
        }
        case FIX_A64_GOT_LO12:
            ins |= (unsigned)((S & 0xFFF) >> 3) << 10;          /* ldr x: 8-byte units */
            break;
        default:
            break;
    }
    for (int i = 0; i < 4; i++) at[i] = (unsigned char)(ins >> (8 * i));
}
