#ifndef BACKEND_H
#define BACKEND_H

#include "cc89.h"

/*
 * A back-end reads the typed AST (struct program, see cc89.h) and
 * writes code for one target. Each target is one function here.
 */

/* x64 (Windows or Linux, by prog->target): writes assembly text
   (Intel syntax) that asm_x64.c encodes */
void backend_x64(struct program* prog, struct text* out);

/* AArch64 (Linux or macOS, by prog->target) */
void backend_arm64(struct program* prog, struct text* out);

/* shared: a static object as .data/.bss directives (backend_data.c) */
void gen_data(struct text* out, struct obj* v);

#endif
