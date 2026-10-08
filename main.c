/*
 * cc89: C89 source -> executable, with no external tools.
 *
 *     cc89 a.c [b.c ...] [-o out] [-target=T] [-l lib ...] [-S]
 *
 *     -target=T   win64 (default on Windows), linux-x64 (default on Linux)
 *                 or macos-arm64 (default on macOS)
 *     -S          also write the assembly text to out.s (to read it)
 *     -g          debug information (DWARF): gdb (Linux) or lldb (macOS,
 *                 in out.dSYM) shows lines, functions, parameters,
 *                 variables and types
 *     -l lib      also import from lib: a DLL on Windows (x.dll),
 *                 a shared library on Linux (libx.so.N).
 *                 Always used: kernel32.dll and a C runtime on Windows,
 *                 libc.so.6 and libm.so.6 on Linux.
 *
 * Pipeline:
 *     lexer.c  parser.c   front-end: source -> typed AST
 *     backend_x64.c       AST -> assembly text
 *       abi_win64.c         Microsoft calling convention
 *       abi_sysv.c          System V calling convention
 *     asm_x64.c           assembly text -> machine code (object)
 *     backend_arm64.c     AST -> AArch64 assembly (macOS)
 *     asm_arm64.c         AArch64 assembly -> machine code
 *     pe.c                object + DLLs -> .exe (Windows)
 *     elf.c               object + shared libraries -> ELF (Linux)
 *     macho.c             object + dylibs -> Mach-O (macOS arm64)
 */
#include "backend.h"
#include "object.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ------------------------------------------------------------- utilities */

void* cc_alloc(size_t size)
{
    void* p = calloc(1, size ? size : 1);
    if (p == NULL)
    {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return p;
}

char* cc_strndup(const char* s, int n)
{
    char* p = cc_alloc(n + 1);
    memcpy(p, s, n);
    return p;
}

static void diagnostic(const char* kind, const char* file, int line, int col, const char* fmt, va_list ap)
{
    fprintf(stderr, "%s:%d:%d: %s: ", file ? file : "", line, col, kind);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
}

void cc_error(const char* file, int line, int col, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diagnostic("error", file, line, col, fmt, ap);
    va_end(ap);
    exit(1);
}

void cc_warning(const char* file, int line, int col, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diagnostic("warning", file, line, col, fmt, ap);
    va_end(ap);
}

void cc_error_at(const struct token* tok, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diagnostic("error", tok->file, tok->line, tok->col, fmt, ap);
    va_end(ap);
    exit(1);
}

void text_append(struct text* t, const char* s, int n)
{
    if (t->size + n + 1 > t->capacity)
    {
        t->capacity = (t->size + n + 1) * 2;
        t->data = realloc(t->data, t->capacity);
        if (t->data == NULL)
        {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
    }
    memcpy(t->data + t->size, s, n);
    t->size += n;
    t->data[t->size] = 0;
}

void text_printf(struct text* t, const char* fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    text_append(t, buf, n);
}

/* ------------------------------------------------------------------ main */

static void usage(void)
{
    fprintf(stderr, "usage: cc89 file.c [file.c ...] [-o out] [-target=win64|linux-x64|macos-arm64] [-l lib] [-S] [-g] [-map]\n");
    exit(1);
}

int main(int argc, char** argv)
{
    const char* inputs[256];
    int ninputs = 0;
    const char* libs[64];
    int nlibs = 0;
    const char* output = NULL;
    int write_asm = 0;
    int write_map = 0;
    int debug = 0;
#ifdef _WIN32
    const struct target* target = find_target("win64");
#elif defined __APPLE__
    const struct target* target = find_target("macos-arm64");
#else
    const struct target* target = find_target("linux-x64");
#endif

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) output = argv[++i];
        else if (!strcmp(argv[i], "-l") && i + 1 < argc && nlibs < 60) libs[nlibs++] = argv[++i];
        else if (!strcmp(argv[i], "-S")) write_asm = 1;
        else if (!strcmp(argv[i], "-map")) write_map = 1;
        else if (!strcmp(argv[i], "-g")) debug = 1;
        else if (!strncmp(argv[i], "-target=", 8))
        {
            target = find_target(argv[i] + 8);
            if (target == NULL || target->kind == TARGET_LINUX_ARM64)
            {
                fprintf(stderr, "unsupported target '%s' (win64, linux-x64, macos-arm64)\n", argv[i] + 8);
                return 1;
            }
        }
        else if (argv[i][0] == '-') usage();
        else if (ninputs < 256) inputs[ninputs++] = argv[i];
    }
    if (ninputs == 0)
        usage();

    char default_out[1024];
    if (output == NULL)
    {
        snprintf(default_out, sizeof default_out, "%s", inputs[0]);
        char* dot = strrchr(default_out, '.');
        if (dot) *dot = 0;
        if (target->kind == TARGET_WIN64)
            strncat(default_out, ".exe", sizeof default_out - strlen(default_out) - 1);
        output = default_out;
    }

    /* front-end: every unit goes into the same program */
    struct program prog = { 0 };
    prog.debug = debug;
    for (int i = 0; i < ninputs; i++)
    {
        struct token_list tokens = { 0 };
        lex_file(inputs[i], &tokens, target);
        parse_program(&tokens, &prog, i, target);
    }

    /* back-end */
    struct text asm_text = { 0 };
    if (target->is_arm64)
        backend_arm64(&prog, &asm_text);
    else
        backend_x64(&prog, &asm_text);

    if (write_asm)
    {
        char path[1024];
        snprintf(path, sizeof path, "%s", output);
        char* dot = strrchr(path, '.');
        if (dot && !strpbrk(dot, "/\\")) *dot = 0;
        strncat(path, ".s", sizeof path - strlen(path) - 1);
        FILE* f = fopen(path, "wb");
        if (f)
        {
            fwrite(asm_text.data, 1, asm_text.size, f);
            fclose(f);
        }
    }

    /* assembler and linker */
    struct object* obj = cc_alloc(sizeof *obj);
    obj->arm64 = target->is_arm64;
    assemble(asm_text.data, obj);
    if (target->kind == TARGET_MACOS_ARM64)
    {
        char map[1024];
        snprintf(map, sizeof map, "%s.map", output);
        link_macho(obj, libs, nlibs, output, write_map ? map : NULL);
    }
    else if (target->kind == TARGET_WIN64)
    {
        const char* dlls[64] = { "kernel32.dll" };
        for (int i = 0; i < nlibs; i++)
            dlls[i + 1] = libs[i];
        link_exe(obj, dlls, nlibs + 1, output);
    }
    else
    {
        const char* so[64] = { "libc.so.6", "libm.so.6" };
        for (int i = 0; i < nlibs; i++)
            so[i + 2] = libs[i];
        char map[1024];
        snprintf(map, sizeof map, "%s.map", output);
        link_elf(obj, so, nlibs + 2, output, write_map ? map : NULL);
    }
    return 0;
}
