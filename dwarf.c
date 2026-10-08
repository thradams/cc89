/*
 * DWARF 4 debug information: .debug_abbrev and .debug_info.
 *
 * .debug_info is a tree of DIEs (debugging information entries):
 *
 *     compile unit
 *        subprogram main            (name, line, address range, frame base)
 *           formal_parameter argc   (type, location = frame base + offset)
 *           variable i
 *        variable counter           (location = fixed address)
 *        base_type int
 *        pointer_type -> ...
 *        structure_type node
 *           member value
 *
 * Each DIE starts with an abbreviation code; the abbreviation (in
 * .debug_abbrev) says the tag and the list of attributes with their
 * encodings (forms). Here the abbreviation table is fixed.
 *
 * The bytes are built in memory and written as ".byte" lines. Addresses
 * are written as ".quad label" and resolved by the linker. A reference
 * to a type is the offset of its DIE from the start of the unit; types
 * are written after the functions and the references are patched.
 */
#include "dwarf.h"
#include <stdlib.h>
#include <string.h>

/* tags */
#define TAG_array_type        0x01
#define TAG_formal_parameter  0x05
#define TAG_member            0x0d
#define TAG_pointer_type      0x0f
#define TAG_compile_unit      0x11
#define TAG_structure_type    0x13
#define TAG_subroutine_type   0x15
#define TAG_union_type        0x17
#define TAG_subrange_type     0x21
#define TAG_base_type         0x24
#define TAG_subprogram        0x2e
#define TAG_variable          0x34

/* attributes */
#define AT_location           0x02
#define AT_name               0x03
#define AT_byte_size          0x0b
#define AT_bit_size           0x0d
#define AT_stmt_list          0x10
#define AT_low_pc             0x11
#define AT_high_pc            0x12
#define AT_language           0x13
#define AT_comp_dir           0x1b
#define AT_producer           0x25
#define AT_prototyped         0x27
#define AT_count              0x37
#define AT_data_member_location 0x38
#define AT_decl_file          0x3a
#define AT_decl_line          0x3b
#define AT_declaration        0x3c
#define AT_encoding           0x3e
#define AT_external           0x3f
#define AT_frame_base         0x40
#define AT_type               0x49
#define AT_data_bit_offset    0x6b

/* forms */
#define FORM_addr             0x01
#define FORM_data2            0x05
#define FORM_data4            0x06
#define FORM_string           0x08
#define FORM_data1            0x0b
#define FORM_flag             0x0c
#define FORM_ref4             0x13
#define FORM_sec_offset       0x17
#define FORM_exprloc          0x18
#define FORM_flag_present     0x19

/* base type encodings */
#define ATE_float             0x04
#define ATE_signed            0x05
#define ATE_signed_char       0x06
#define ATE_unsigned          0x07
#define ATE_unsigned_char     0x08

/* location expressions */
#define OP_addr               0x03
#define OP_fbreg              0x91
#define OP_breg0              0x70

enum abbrev
{
    AB_CU = 1, AB_BASE, AB_PTR, AB_PTR_VOID, AB_STRUCT, AB_UNION, AB_MEMBER, AB_MEMBER_BITS,
    AB_ARRAY, AB_SUBRANGE, AB_SUBRANGE_UNKNOWN, AB_FUNC_TYPE, AB_FUNC_TYPE_VOID,
    AB_SUBPROGRAM, AB_SUBPROGRAM_VOID, AB_PARAM, AB_LOCAL, AB_GLOBAL,
    AB_STRUCT_DECL, AB_UNION_DECL
};

/* the abbreviation table: code, tag, has children, then attribute/form pairs */
static const int abbrevs[][24] = {
    { AB_CU, TAG_compile_unit, 1, AT_producer, FORM_string, AT_language, FORM_data2, AT_name, FORM_string,
      AT_comp_dir, FORM_string, AT_low_pc, FORM_addr, AT_high_pc, FORM_addr, AT_stmt_list, FORM_sec_offset, 0 },
    { AB_BASE, TAG_base_type, 0, AT_name, FORM_string, AT_encoding, FORM_data1, AT_byte_size, FORM_data1, 0 },
    { AB_PTR, TAG_pointer_type, 0, AT_byte_size, FORM_data1, AT_type, FORM_ref4, 0 },
    { AB_PTR_VOID, TAG_pointer_type, 0, AT_byte_size, FORM_data1, 0 },
    { AB_STRUCT, TAG_structure_type, 1, AT_name, FORM_string, AT_byte_size, FORM_data4, 0 },
    { AB_UNION, TAG_union_type, 1, AT_name, FORM_string, AT_byte_size, FORM_data4, 0 },
    { AB_MEMBER, TAG_member, 0, AT_name, FORM_string, AT_type, FORM_ref4, AT_data_member_location, FORM_data4, 0 },
    { AB_MEMBER_BITS, TAG_member, 0, AT_name, FORM_string, AT_type, FORM_ref4, AT_bit_size, FORM_data1,
      AT_data_bit_offset, FORM_data4, 0 },
    { AB_ARRAY, TAG_array_type, 1, AT_type, FORM_ref4, 0 },
    { AB_SUBRANGE, TAG_subrange_type, 0, AT_count, FORM_data4, 0 },
    { AB_SUBRANGE_UNKNOWN, TAG_subrange_type, 0, 0 },
    { AB_FUNC_TYPE, TAG_subroutine_type, 0, AT_prototyped, FORM_flag_present, AT_type, FORM_ref4, 0 },
    { AB_FUNC_TYPE_VOID, TAG_subroutine_type, 0, AT_prototyped, FORM_flag_present, 0 },
    { AB_SUBPROGRAM, TAG_subprogram, 1, AT_external, FORM_flag, AT_name, FORM_string, AT_decl_file, FORM_data2,
      AT_decl_line, FORM_data4, AT_type, FORM_ref4, AT_low_pc, FORM_addr, AT_high_pc, FORM_addr,
      AT_frame_base, FORM_exprloc, AT_prototyped, FORM_flag_present, 0 },
    { AB_SUBPROGRAM_VOID, TAG_subprogram, 1, AT_external, FORM_flag, AT_name, FORM_string, AT_decl_file, FORM_data2,
      AT_decl_line, FORM_data4, AT_low_pc, FORM_addr, AT_high_pc, FORM_addr,
      AT_frame_base, FORM_exprloc, AT_prototyped, FORM_flag_present, 0 },
    { AB_PARAM, TAG_formal_parameter, 0, AT_name, FORM_string, AT_decl_file, FORM_data2, AT_decl_line, FORM_data4,
      AT_type, FORM_ref4, AT_location, FORM_exprloc, 0 },
    { AB_LOCAL, TAG_variable, 0, AT_name, FORM_string, AT_decl_file, FORM_data2, AT_decl_line, FORM_data4,
      AT_type, FORM_ref4, AT_location, FORM_exprloc, 0 },
    { AB_GLOBAL, TAG_variable, 0, AT_name, FORM_string, AT_decl_file, FORM_data2, AT_decl_line, FORM_data4,
      AT_type, FORM_ref4, AT_external, FORM_flag, AT_location, FORM_exprloc, 0 },
    { AB_STRUCT_DECL, TAG_structure_type, 0, AT_name, FORM_string, AT_declaration, FORM_flag_present, 0 },
    { AB_UNION_DECL, TAG_union_type, 0, AT_name, FORM_string, AT_declaration, FORM_flag_present, 0 },
};

struct patch
{
    int pos;
    int type_index;         /* type reference: index in dwarf.types, or -1 */
    const char* label;      /* address: label, or NULL */
};

struct type_die
{
    struct type* type;
    int offset;             /* -1 until written */
};

struct dwarf
{
    struct text* out;
    const char* files[256];
    int nfiles;

    unsigned char* buf;
    int size, capacity;

    struct patch* patches;
    int npatches, patch_capacity;

    struct type_die* types;
    int ntypes, type_capacity;
};

struct dwarf* dwarf_new(struct text* out)
{
    struct dwarf* dw = cc_alloc(sizeof *dw);
    dw->out = out;
    return dw;
}

int dwarf_file(struct dwarf* dw, const char* name)
{
    for (int i = 0; i < dw->nfiles; i++)
        if (strcmp(dw->files[i], name) == 0)
            return i + 1;
    if (dw->nfiles == 255)
        return 255;
    dw->files[dw->nfiles++] = name;
    text_printf(dw->out, "    .file %d \"%s\"\n", dw->nfiles, name);
    return dw->nfiles;
}

/* ---------------------------------------------------------------- bytes */

static void u8(struct dwarf* dw, int v)
{
    if (dw->size == dw->capacity)
    {
        dw->capacity = dw->capacity * 2 + 1024;
        dw->buf = realloc(dw->buf, dw->capacity);
        if (dw->buf == NULL)
        {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
    }
    dw->buf[dw->size++] = (unsigned char)v;
}

static void u16(struct dwarf* dw, int v) { u8(dw, v); u8(dw, v >> 8); }
static void u32(struct dwarf* dw, unsigned v) { for (int i = 0; i < 4; i++) u8(dw, (int)(v >> (8 * i))); }

static void uleb(struct dwarf* dw, unsigned long long v)
{
    do
    {
        int b = v & 0x7F;
        v >>= 7;
        u8(dw, v ? b | 0x80 : b);
    } while (v);
}

static void sleb(struct dwarf* dw, long long v)
{
    for (;;)
    {
        int b = v & 0x7F;
        v >>= 7;   /* arithmetic shift keeps the sign */
        int done = (v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40));
        u8(dw, done ? b : b | 0x80);
        if (done) break;
    }
}

static void str(struct dwarf* dw, const char* s)
{
    while (*s) u8(dw, *s++);
    u8(dw, 0);
}

static void add_patch(struct dwarf* dw, int type_index, const char* label)
{
    if (dw->npatches == dw->patch_capacity)
    {
        dw->patch_capacity = dw->patch_capacity * 2 + 64;
        dw->patches = realloc(dw->patches, sizeof(struct patch) * dw->patch_capacity);
    }
    struct patch* p = &dw->patches[dw->npatches++];
    p->pos = dw->size;
    p->type_index = type_index;
    p->label = label;
}

/* an address: 8 bytes, written later as .quad label */
static void addr(struct dwarf* dw, const char* label)
{
    add_patch(dw, -1, label);
    for (int i = 0; i < 8; i++) u8(dw, 0);
}

/* a reference to the DIE of a type (offset in the unit), patched at the end */
static void type_ref(struct dwarf* dw, struct type* t)
{
    int index = -1;
    for (int i = dw->ntypes - 1; i >= 0 && index < 0; i--)
        if (dw->types[i].type == t) index = i;
    if (index < 0)
    {
        if (dw->ntypes == dw->type_capacity)
        {
            dw->type_capacity = dw->type_capacity * 2 + 64;
            dw->types = realloc(dw->types, sizeof(struct type_die) * dw->type_capacity);
        }
        dw->types[dw->ntypes].type = t;
        dw->types[dw->ntypes].offset = -1;
        index = dw->ntypes++;
    }
    add_patch(dw, index, NULL);
    u32(dw, 0);
}

static void decl_position(struct dwarf* dw, const struct token* tok)
{
    u16(dw, tok ? dwarf_file(dw, tok->file) : 0);
    u32(dw, tok ? (unsigned)tok->line : 0);
}

/* ---------------------------------------------------------------- types */

static const char* base_name(struct type* t)
{
    switch (t->kind)
    {
        case TY_CHAR: return t->is_unsigned ? "unsigned char" : "char";
        case TY_SHORT: return t->is_unsigned ? "unsigned short" : "short";
        case TY_INT: return t->is_unsigned ? "unsigned int" : "int";
        case TY_LONG: return t->is_unsigned ? "unsigned long" : "long";
        case TY_LLONG: return t->is_unsigned ? "unsigned long long" : "long long";
        case TY_FLOAT: return "float";
        case TY_DOUBLE: return "double";
        default: return "long double";
    }
}

static int is_void(struct type* t)
{
    return t == NULL || t->kind == TY_VOID;
}

static void write_type(struct dwarf* dw, struct type* t)
{
    switch (t->kind)
    {
        case TY_PTR:
            if (is_void(t->base))
            {
                uleb(dw, AB_PTR_VOID);
                u8(dw, 8);
            }
            else
            {
                uleb(dw, AB_PTR);
                u8(dw, 8);
                type_ref(dw, t->base);
            }
            return;

        case TY_ARRAY:
            uleb(dw, AB_ARRAY);
            type_ref(dw, t->base);
            if (t->array_len >= 0)
            {
                uleb(dw, AB_SUBRANGE);
                u32(dw, (unsigned)t->array_len);
            }
            else
                uleb(dw, AB_SUBRANGE_UNKNOWN);
            u8(dw, 0);   /* end of children */
            return;

        case TY_FUNC:
            if (is_void(t->base))
                uleb(dw, AB_FUNC_TYPE_VOID);
            else
            {
                uleb(dw, AB_FUNC_TYPE);
                type_ref(dw, t->base);
            }
            return;

        case TY_STRUCT:
        case TY_UNION:
        {
            /* an incomplete struct is only a declaration; gdb finds the
               complete one by the tag */
            if (!t->is_complete)
            {
                uleb(dw, t->kind == TY_STRUCT ? AB_STRUCT_DECL : AB_UNION_DECL);
                str(dw, t->tag ? t->tag : "");
                return;
            }
            uleb(dw, t->kind == TY_STRUCT ? AB_STRUCT : AB_UNION);
            str(dw, t->tag ? t->tag : "");
            u32(dw, (unsigned)t->size);
            for (struct member* m = t->members; m; m = m->next)
            {
                if (m->name == NULL)
                    continue;
                if (m->is_bitfield)
                {
                    uleb(dw, AB_MEMBER_BITS);
                    str(dw, m->name);
                    type_ref(dw, m->type);
                    u8(dw, m->bit_width);
                    u32(dw, (unsigned)(m->offset * 8 + m->bit_offset));
                }
                else
                {
                    uleb(dw, AB_MEMBER);
                    str(dw, m->name);
                    type_ref(dw, m->type);
                    u32(dw, (unsigned)m->offset);
                }
            }
            u8(dw, 0);
            return;
        }

        default:
        {
            int enc;
            if (type_is_float(t)) enc = ATE_float;
            else if (t->kind == TY_CHAR) enc = t->is_unsigned ? ATE_unsigned_char : ATE_signed_char;
            else enc = t->is_unsigned ? ATE_unsigned : ATE_signed;
            uleb(dw, AB_BASE);
            str(dw, base_name(t));
            u8(dw, enc);
            u8(dw, t->size);
            return;
        }
    }
}

/* ---------------------------------------------------------------- DIEs */

static void write_variable(struct dwarf* dw, struct obj* v, int abbrev, int frame_reg)
{
    (void)frame_reg;
    uleb(dw, abbrev);
    str(dw, v->name);
    decl_position(dw, v->tok);
    type_ref(dw, v->type);
    if (abbrev == AB_GLOBAL)
    {
        u8(dw, !v->is_static);
        /* location: the address of the object */
        uleb(dw, 9);
        u8(dw, OP_addr);
        addr(dw, v->label);
        return;
    }
    /* location: frame base + offset */
    struct dwarf tmp = { 0 };
    sleb(&tmp, v->offset);
    uleb(dw, 1 + tmp.size);
    u8(dw, OP_fbreg);
    for (int i = 0; i < tmp.size; i++) u8(dw, tmp.buf[i]);
    free(tmp.buf);
}

static void write_function(struct dwarf* dw, struct obj* fn, int frame_reg)
{
    struct type* ret = fn->type->base;
    char end[300];
    snprintf(end, sizeof end, ".Lend.%s", fn->label);

    uleb(dw, is_void(ret) ? AB_SUBPROGRAM_VOID : AB_SUBPROGRAM);
    u8(dw, !fn->is_static);
    str(dw, fn->name);
    decl_position(dw, fn->tok);
    if (!is_void(ret))
        type_ref(dw, ret);
    addr(dw, fn->label);
    addr(dw, cc_strndup(end, (int)strlen(end)));
    /* frame base: the frame pointer register itself (breg N 0) */
    uleb(dw, 2);
    u8(dw, OP_breg0 + frame_reg);
    u8(dw, 0);

    for (struct obj* v = fn->params; v; v = v->next)
        write_variable(dw, v, AB_PARAM, frame_reg);
    for (struct obj* v = fn->locals; v; v = v->next_local)
        if (v->param_index < 0)
            write_variable(dw, v, AB_LOCAL, frame_reg);
    u8(dw, 0);   /* end of children */
}

/* ------------------------------------------------------------- output */

static void write_bytes(struct text* out, const unsigned char* p, int n)
{
    for (int i = 0; i < n; i += 16)
    {
        text_printf(out, "    .byte %d", p[i]);
        for (int j = i + 1; j < n && j < i + 16; j++)
            text_printf(out, ",%d", p[j]);
        text_printf(out, "\n");
    }
}

void dwarf_emit(struct dwarf* dw, struct program* prog, int frame_reg,
                const char* text_begin, const char* text_end)
{
    struct text* out = dw->out;

    /* .debug_abbrev: the fixed table */
    text_printf(out, "\n    .section .debug_abbrev\n");
    struct dwarf ab = { 0 };
    for (int i = 0; i < (int)(sizeof abbrevs / sizeof abbrevs[0]); i++)
    {
        const int* a = abbrevs[i];
        uleb(&ab, a[0]);
        uleb(&ab, a[1]);
        u8(&ab, a[2]);
        for (int j = 3; a[j]; j += 2)
        {
            uleb(&ab, a[j]);
            uleb(&ab, a[j + 1]);
        }
        u8(&ab, 0);
        u8(&ab, 0);
    }
    u8(&ab, 0);
    write_bytes(out, ab.buf, ab.size);
    free(ab.buf);

    /* .debug_info: unit header */
    u32(dw, 0);              /* unit length, patched below */
    u16(dw, 4);              /* DWARF version */
    u32(dw, 0);              /* offset of the abbreviations */
    u8(dw, 8);               /* address size */

    uleb(dw, AB_CU);
    str(dw, "cc89");
    u16(dw, 0x0001);         /* language: C89 */
    str(dw, dw->nfiles ? dw->files[0] : "");
    str(dw, "");
    addr(dw, text_begin);
    addr(dw, text_end);
    u32(dw, 0);              /* offset in .debug_line */

    for (struct obj* v = prog->globals; v; v = v->next)
    {
        if (v->is_function && v->body)
            write_function(dw, v, frame_reg);
        else if (!v->is_function && v->is_defined && v->name[0] != '.')
            write_variable(dw, v, AB_GLOBAL, frame_reg);
    }

    /* the types; writing one may add others to the list */
    for (int i = 0; i < dw->ntypes; i++)
    {
        dw->types[i].offset = dw->size;
        write_type(dw, dw->types[i].type);
    }
    u8(dw, 0);               /* end of the unit's children */

    unsigned length = (unsigned)dw->size - 4;
    for (int i = 0; i < 4; i++) dw->buf[i] = (unsigned char)(length >> (8 * i));

    for (int i = 0; i < dw->npatches; i++)
    {
        struct patch* p = &dw->patches[i];
        if (p->type_index < 0)
            continue;
        int offset = dw->types[p->type_index].offset;
        for (int b = 0; b < 4; b++) dw->buf[p->pos + b] = (unsigned char)(offset >> (8 * b));
    }

    /* write: bytes, and ".quad label" where an address goes */
    text_printf(out, "\n    .section .debug_info\n");
    int at = 0;
    for (int i = 0; i < dw->npatches; i++)
    {
        struct patch* p = &dw->patches[i];
        if (p->label == NULL)
            continue;
        write_bytes(out, dw->buf + at, p->pos - at);
        text_printf(out, "    .quad %s\n", p->label);
        at = p->pos + 8;
    }
    write_bytes(out, dw->buf + at, dw->size - at);
}
