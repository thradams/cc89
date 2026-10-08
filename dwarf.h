#ifndef DWARF_H
#define DWARF_H

/*
 * Debug information (-g), DWARF version 4.
 *
 * The back-end calls dwarf_file() and writes ".loc file line" before
 * the code of each statement; the assembler turns those into rows of
 * the line table, which the linker encodes in .debug_line.
 *
 * dwarf_emit() writes .debug_abbrev and .debug_info (functions,
 * parameters, variables and their types) as assembly directives.
 */
#include "cc89.h"

struct dwarf;

struct dwarf* dwarf_new(struct text* out);

/* number of the file in the line table; writes ".file N" the first time */
int dwarf_file(struct dwarf* dw, const char* name);

/*
 * frame_reg: DWARF number of the frame pointer (rbp = 6 on x64).
 * Locals and parameters are at frame_reg + obj->offset.
 * Each function must have the labels fn->label and ".Lend.<label>".
 */
void dwarf_emit(struct dwarf* dw, struct program* prog, int frame_reg,
                const char* text_begin, const char* text_end);

#endif
