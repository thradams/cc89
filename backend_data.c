/*
 * Static objects (globals, statics, strings) as assembly directives.
 * The same for every instruction set:
 *
 *       .data                    (or .bss when all zero)
 *       .globl counter           (external linkage)
 *       .balign 4
 *   counter:
 *       .byte 5,0,0,0
 *       .quad other+8            (an address: the linker fills it)
 */
#include "backend.h"
#include <string.h>

static int is_string(struct obj* v)
{
    return strncmp(v->label, ".str.", 5) == 0;
}

void gen_data(struct text* out, struct obj* v)
{
    int size = v->type->size;
    int align = v->type->align;

    if (v->data == NULL)
        text_printf(out, "\n    .bss\n");
    else if (is_string(v))
        text_printf(out, "\n    .section .rdata,\"dr\"\n");
    else
        text_printf(out, "\n    .data\n");

    if (!v->is_static)
        text_printf(out, "    .globl %s\n", v->label);
    if (v->is_selectany)
        text_printf(out, "    .selectany %s\n", v->label);   /* other units may define it too */
    text_printf(out, "    .balign %d\n", align);
    text_printf(out, "%s:\n", v->label);

    if (v->data == NULL)
    {
        text_printf(out, "    .zero %d\n", size > 0 ? size : 1);
        return;
    }

    for (int i = 0; i < size;)
    {
        struct reloc* r = v->relocs;
        while (r && r->offset != i) r = r->next;
        if (r)
        {
            text_printf(out, "    .quad %s%+lld\n", r->label, r->addend);
            i += 8;
            continue;
        }
        /* up to 16 bytes per line, stopping before the next address */
        int n = 0;
        text_printf(out, "    .byte %d", v->data[i++]);
        while (i < size && ++n < 16)
        {
            struct reloc* next = v->relocs;
            while (next && next->offset != i) next = next->next;
            if (next) break;
            text_printf(out, ",%d", v->data[i++]);
        }
        text_printf(out, "\n");
    }
}

