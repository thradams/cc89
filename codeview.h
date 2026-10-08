#ifndef CODEVIEW_H
#define CODEVIEW_H

/*
 * Debug information for Windows (-g): CodeView, the format inside .pdb
 * files. The back-end writes two sections, as MSVC does in a .obj:
 *
 *   .debug$T  type records (LF_STRUCTURE, LF_POINTER, LF_PROCEDURE ...)
 *   .debug$S  symbol records (S_GPROC32, S_REGREL32, S_GDATA32 ...)
 *
 * The linker (pdb.c) puts them in the .pdb with the line table, built
 * from the ".loc" lines of the back-end like on Linux.
 */
#include "cc89.h"

struct codeview;

struct codeview* codeview_new(struct text* out);

/* number of the file in the line table; writes ".file N" the first time */
int codeview_file(struct codeview* cv, const char* name);

/* frame_reg: CodeView number of the frame pointer (rbp = 334 on x64) */
void codeview_emit(struct codeview* cv, struct program* prog, int frame_reg);

#endif
