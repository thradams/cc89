/*
 * Targets (spec 3.3). The table is constant; the chosen entry is
 * passed to the lexer, the parser and the back-end.
 */
#include "cc89.h"
#include <string.h>

static const struct target targets[] = {
    /* kind                 name            long ldouble wchar gcc_bf va_list      unsigned char  arm64 */
    { TARGET_WIN64,         "win64",        4,   8,      2,    0,     VA_CHAR_PTR, 0,             0 },
    { TARGET_LINUX_X64,     "linux-x64",    8,   16,     4,    1,     VA_SYSV,     0,             0 },
    { TARGET_MACOS_ARM64,   "macos-arm64",  8,   8,      4,    1,     VA_CHAR_PTR, 0,             1 },
    { TARGET_LINUX_ARM64,   "linux-arm64",  8,   16,     4,    1,     VA_AAPCS,    1,             1 },
};

const struct target* find_target(const char* name)
{
    for (int i = 0; i < (int)(sizeof targets / sizeof targets[0]); i++)
        if (strcmp(targets[i].name, name) == 0)
            return &targets[i];
    return NULL;
}
