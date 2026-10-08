/*
 * Build system for cc89
 *
 * WINDOWS
 *   cl build.c && build
 *
 * LINUX/MACOS
 *   cc build.c -o build && ./build
 *
 * OPTIONS
 *   ./build          (release)
 *   ./build debug    (no optimization, debug info)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined _MSC_VER && !defined __clang__
#define COMPILER_MSVC
#endif

#define SOURCE_FILES       \
    " main.c "             \
    " lexer.c "            \
    " type.c "             \
    " target.c "           \
    " parser.c "           \
    " backend_x64.c "      \
    " backend_arm64.c "    \
    " backend_data.c "     \
    " abi_win64.c "        \
    " abi_sysv.c "         \
    " asm.c "              \
    " asm_x64.c "          \
    " asm_arm64.c "        \
    " pe.c "               \
    " elf.c "              \
    " macho.c "            \
    " dwarf.c "            \
    " codeview.c "         \
    " pdb.c "

#if defined COMPILER_MSVC
#define CC             "cl "
#define DEBUG_FLAGS    " /nologo /W3 /D_CRT_SECURE_NO_WARNINGS /Od /Zi /MDd /RTC1 "
#define RELEASE_FLAGS  " /nologo /W3 /D_CRT_SECURE_NO_WARNINGS /O2 /MT "
#define OUTPUT         " /Fe:cc89.exe "
#else
#define CC             "cc "
#define DEBUG_FLAGS    " -std=c99 -Wall -D_CRT_SECURE_NO_WARNINGS -O0 -g "
#define RELEASE_FLAGS  " -std=c99 -Wall -D_CRT_SECURE_NO_WARNINGS -O2 "
#define OUTPUT         " -o cc89 "
#endif

static int execute_cmd(const char* cmd)
{
    printf("%s\n", cmd);
    fflush(stdout);
    return system(cmd);
}

int main(int argc, char* argv[])
{
    int debug = 0;
    int result = 0;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "debug") == 0)
        {
            debug = 1;
        }
        else
        {
            printf("unrecognized option: %s\n", argv[i]);
            printf("usage: %s [debug]\n", argv[0]);
            result = 1;
        }
    }

    if (result == 0)
    {
        if (debug)
        {
            result = execute_cmd(CC DEBUG_FLAGS SOURCE_FILES OUTPUT);
        }
        else
        {
            result = execute_cmd(CC RELEASE_FLAGS SOURCE_FILES OUTPUT);
        }
    }

    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
