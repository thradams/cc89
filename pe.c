/*
 * Linker: object -> Windows x64 executable (PE32+).
 *
 * Symbols that the program does not define are looked up in the export
 * tables of DLLs, read from the files themselves. No import library
 * (.lib) is needed.
 *
 * C runtime: the program uses ucrtbase.dll (the CRT of current MSVC,
 * used by cake -target=msvc-win-x64) when every C function it calls is
 * there; otherwise msvcrt.dll (used by tcc, which exports printf and
 * friends directly). Never both: each one has its own FILE and heap.
 * The linker also adds the startup code (crt0) of the chosen runtime.
 *
 * Image layout (no relocations: the image always loads at its base):
 *
 *     headers        DOS header, PE signature, COFF header,
 *                    optional header, section table
 *     .text          code, then one 6-byte thunk per imported function:
 *                        jmp qword ptr [rip+IAT entry]
 *     .data          data (strings, globals, zero-filled objects)
 *     .idata         import directory, lookup tables, IAT, names
 *
 * A call to an imported function calls its thunk (and its address is the
 * thunk address too); the loader writes the
 * real address in the IAT entry when the program starts.
 */
#include "object.h"
#include "cc89.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IMAGE_BASE        0x140000000ull
#define SECTION_ALIGN     0x1000
#define FILE_ALIGN        0x200
#define HEADERS_SIZE      0x400

struct dll
{
    const char* name;           /* as written in the import table */
    char** exports;
    int nexports;
};

struct import
{
    struct import* next;
    const char* name;           /* symbol used by the program */
    const char* import_name;    /* name exported by the DLL */
    struct dll* dll;
    int thunk_offset;           /* inside .text */
    int iat_rva;
};

static unsigned rd16(const unsigned char* p) { return p[0] | p[1] << 8; }
static unsigned rd32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

static void wr16(unsigned char* p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void wr32(unsigned char* p, unsigned v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i)); }
static void wr64(unsigned char* p, unsigned long long v) { for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i)); }

static int align_up(int n, int a)
{
    return (n + a - 1) / a * a;
}

/* ---------------------------------------------------------- DLL exports */

static unsigned char* read_all(const char* path, long* size)
{
    FILE* f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* buf = malloc(*size);
    if (buf)
        fread(buf, 1, *size, f);
    fclose(f);
    return buf;
}

/* RVA -> offset in the file, using the section table */
static long rva_to_file(const unsigned char* img, unsigned rva)
{
    const unsigned char* coff = img + rd32(img + 0x3C) + 4;
    int nsections = rd16(coff + 2);
    const unsigned char* sec = coff + 20 + rd16(coff + 16);
    for (int i = 0; i < nsections; i++, sec += 40)
    {
        unsigned va = rd32(sec + 12), vsize = rd32(sec + 8), raw = rd32(sec + 20);
        if (rva >= va && rva < va + vsize)
            return (long)(rva - va + raw);
    }
    return -1;
}

/*
 * The export directory (data directory 0) has the list of names:
 *   +24 NumberOfNames   +32 AddressOfNames (RVAs of the name strings)
 */
static void load_dll(struct dll* dll, const char* arg)
{
    char path[1024];
    const char* base = arg;
    for (const char* c = arg; *c; c++)
        if (*c == '/' || *c == '\\') base = c + 1;
    dll->name = base;

    long size = 0;
    unsigned char* img = NULL;
    if (base == arg)
    {
        /* plain name: Sysnative (the 64-bit System32 seen from a 32-bit cc89), System32, then the current directory */
        const char* root = getenv("SystemRoot");
        snprintf(path, sizeof path, "%s\\Sysnative\\%s", root ? root : "C:\\Windows", arg);
        img = read_all(path, &size);
        if (img == NULL)
        {
            snprintf(path, sizeof path, "%s\\System32\\%s", root ? root : "C:\\Windows", arg);
            img = read_all(path, &size);
        }
    }
    if (img == NULL)
        img = read_all(arg, &size);
    if (img == NULL || size < 0x40 || img[0] != 'M' || img[1] != 'Z')
        cc_error(arg, 0, 0, "cannot read DLL");

    const unsigned char* opt = img + rd32(img + 0x3C) + 24;
    if (rd16(opt) != 0x20B)
        cc_error(arg, 0, 0, "not a 64-bit DLL");
    unsigned export_rva = rd32(opt + 112);        /* data directory 0 */
    long dir = rva_to_file(img, export_rva);
    if (export_rva == 0 || dir < 0)
    {
        free(img);
        return;
    }
    int n = (int)rd32(img + dir + 24);
    long names = rva_to_file(img, rd32(img + dir + 32));
    dll->exports = cc_alloc(sizeof(char*) * (n > 0 ? n : 1));
    for (int i = 0; i < n; i++)
    {
        long s = rva_to_file(img, rd32(img + names + 4 * i));
        dll->exports[i] = cc_strndup((const char*)img + s, (int)strlen((const char*)img + s));
    }
    dll->nexports = n;
    free(img);
}

/*
 * Some C functions live in the static part of the Microsoft CRT (the
 * .lib), not in ucrtbase.dll. They are thin wrappers, so we import the
 * function they wrap instead.
 */
static const char* crt_alias(const char* name)
{
    static const char* aliases[][2] = {
        { "atexit", "_crt_atexit" },
        { 0, 0 }
    };
    for (int i = 0; aliases[i][0]; i++)
        if (strcmp(aliases[i][0], name) == 0)
            return aliases[i][1];
    return name;
}

/*
 * Startup code, assembled into the program by the linker.
 * At the entry point rsp is 16-aligned minus 8 (the return address).
 * main (or wmain) is called with argc/argv, then exit flushes stdio and
 * runs the atexit functions.
 */
static const char* crt0_ucrt =
    "    .text\n"
    "    .func __cc89_start\n"
    "__cc89_start:\n"
    "    sub rsp, 56                  # 32 home area + 8 argc + 8 alignment\n"
    "    mov rcx, 1                   # _crt_argv_unexpanded_arguments\n"
    "    call _configure_%s_argv\n"
    "    call __p___argc\n"
    "    movsxd rax, dword ptr [rax]\n"
    "    mov qword ptr [rsp+32], rax\n"
    "    call __p___%sargv\n"
    "    mov rdx, qword ptr [rax]     # argv\n"
    "    mov rcx, qword ptr [rsp+32]  # argc\n"
    "    call %s\n"
    "    mov ecx, eax\n"
    "    call exit\n"
    "    .endfunc\n";

/* msvcrt: __getmainargs(&argc, &argv, &env, 0, &startinfo) */
static const char* crt0_msvcrt =
    "    .text\n"
    "    .func __cc89_start\n"
    "__cc89_start:\n"
    "    sub rsp, 72                  # 32 home + 8 arg5 + argc argv env startinfo\n"
    "    lea rcx, [rsp+40]\n"
    "    lea rdx, [rsp+48]\n"
    "    lea r8, [rsp+56]\n"
    "    mov r9, 0                    # no wildcard expansion\n"
    "    mov dword ptr [rsp+64], 0    # startinfo.newmode\n"
    "    lea rax, [rsp+64]\n"
    "    mov qword ptr [rsp+32], rax  # 5th argument on the stack\n"
    "    call __%sgetmainargs\n"
    "    movsxd rcx, dword ptr [rsp+40]\n"
    "    mov rdx, qword ptr [rsp+48]\n"
    "    mov r8, qword ptr [rsp+56]\n"
    "    call %s\n"
    "    mov ecx, eax\n"
    "    call exit\n"
    "    .endfunc\n";

static struct dll* find_in_dlls(struct dll* dlls, int n, const char* name)
{
    for (int i = 0; i < n; i++)
        for (int j = 0; j < dlls[i].nexports; j++)
            if (strcmp(dlls[i].exports[j], name) == 0)
                return &dlls[i];
    return NULL;
}

/* the DLL that exports name (or its CRT alias); sets the imported name */
static struct dll* resolve(struct dll* dlls, int n, const char* name, const char** import_name)
{
    *import_name = name;
    struct dll* dll = find_in_dlls(dlls, n, name);
    if (dll == NULL)
    {
        *import_name = crt_alias(name);
        dll = find_in_dlls(dlls, n, *import_name);
    }
    if (dll == NULL)
    {
        /* oldnames.lib of MSVC: POSIX names (strdup, open, ...) are the
           functions with a leading underscore (_strdup, _open, ...) */
        char buf[256];
        snprintf(buf, sizeof buf, "_%s", name);
        dll = find_in_dlls(dlls, n, buf);
        if (dll)
            *import_name = cc_strndup(buf, (int)strlen(buf));
    }
    return dll;
}

/* ---------------------------------------------------------------- link */

static void put(struct section* s, const void* data, int n)
{
    if (s->size + n > s->capacity)
    {
        s->capacity = (s->size + n) * 2;
        s->data = realloc(s->data, s->capacity);
        if (s->data == NULL)
        {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
    }
    memcpy(s->data + s->size, data, n);
    s->size += n;
}

/* are all undefined symbols exported by one of these DLLs? */
static const char* first_missing(struct object* obj, struct dll* dlls, int n)
{
    for (struct fixup* f = obj->fixups; f; f = f->next)
    {
        struct symbol* s = find_symbol(obj, f->name, 0);
        if (s && s->defined)
            continue;
        const char* import_name;
        if (!resolve(dlls, n, f->name, &import_name))
            return f->name;
    }
    return NULL;
}

void link_exe(struct object* obj, const char** dll_names, int n_names, const char* path)
{
    /* dlls[0] is the C runtime, the others come from the command line */
    int ndlls = n_names + 1;
    struct dll* dlls = cc_alloc(sizeof(struct dll) * ndlls);
    for (int i = 0; i < n_names; i++)
        load_dll(&dlls[i + 1], dll_names[i]);

    struct symbol* wmain = find_symbol(obj, "wmain", 0);
    int wide = wmain && wmain->defined;
    const char* main_name = wide ? "wmain" : "main";
    struct symbol* main_sym = find_symbol(obj, main_name, 0);
    if (main_sym == NULL || !main_sym->defined)
        cc_error(path, 0, 0, "undefined symbol 'main'");

    load_dll(&dlls[0], "ucrtbase.dll");
    const char* missing = first_missing(obj, dlls, ndlls);
    if (missing)
    {
        struct dll ucrt = dlls[0];
        load_dll(&dlls[0], "msvcrt.dll");
        if (first_missing(obj, dlls, ndlls))
        {
            dlls[0] = ucrt;
            cc_error(path, 0, 0, "undefined symbol '%s'", missing);
        }
    }

    char crt0[2048];
    if (strcmp(dlls[0].name, "msvcrt.dll") == 0)
        snprintf(crt0, sizeof crt0, crt0_msvcrt, wide ? "w" : "", main_name);
    else
        snprintf(crt0, sizeof crt0, crt0_ucrt, wide ? "wide" : "narrow", wide ? "w" : "", main_name);
    assemble(crt0, obj);

    struct section* text = &obj->sections[SEC_TEXT];
    struct section* data = &obj->sections[SEC_DATA];

    /* 1. every symbol used but not defined must come from a DLL */
    struct import* imports = NULL;
    struct import** tail = &imports;
    int nimports = 0;
    for (struct fixup* f = obj->fixups; f; f = f->next)
    {
        struct symbol* s = find_symbol(obj, f->name, 1);
        if (s->defined)
            continue;
        const char* import_name;
        struct dll* dll = resolve(dlls, ndlls, f->name, &import_name);
        if (dll == NULL)
            cc_error(path, 0, 0, "undefined symbol '%s'", f->name);

        struct import* im = cc_alloc(sizeof *im);
        im->name = f->name;
        im->import_name = import_name;
        im->dll = dll;
        *tail = im;
        tail = &im->next;
        nimports++;

        /* from now on the symbol is the thunk */
        s->defined = 1;
        s->section = SEC_TEXT;
        s->offset = -1 - (nimports - 1);   /* patched below */
    }

    /* 2. thunks at the end of .text:  FF 25 rel32 = jmp qword ptr [rip+rel32] */
    while (text->size % 16) put(text, (unsigned char[]){ 0xCC }, 1);
    for (struct import* im = imports; im; im = im->next)
    {
        im->thunk_offset = text->size;
        find_symbol(obj, im->name, 0)->offset = text->size;
        put(text, (unsigned char[]){ 0xFF, 0x25, 0, 0, 0, 0 }, 6);
    }

    /* 3. addresses of the sections */
    int text_rva = SECTION_ALIGN;
    int data_rva = align_up(text_rva + text->size, SECTION_ALIGN);
    int idata_rva = align_up(data_rva + (data->size ? data->size : 1), SECTION_ALIGN);

    /*
     * 4. import section, for each DLL:
     *      import descriptor (20 bytes): lookup table RVA, 0, 0, name RVA, IAT RVA
     *    then the lookup tables, the IATs (same content before loading:
     *    RVAs of hint/name entries), the hint/name entries, the DLL names.
     */
    int used_dlls = 0;
    for (int i = 0; i < ndlls; i++)
    {
        int used = 0;
        for (struct import* im = imports; im; im = im->next)
            if (im->dll == &dlls[i]) used = 1;
        used_dlls += used;
    }
    int desc_size = (used_dlls + 1) * 20;
    int table_size = (nimports + used_dlls) * 8;      /* each table ends with 0 */
    int ilt_off = desc_size;
    int iat_off = ilt_off + table_size;
    int names_off = iat_off + table_size;

    int names_size = 0;
    for (struct import* im = imports; im; im = im->next)
        names_size += align_up(2 + (int)strlen(im->import_name) + 1, 2);
    for (int i = 0; i < ndlls; i++)
        names_size += (int)strlen(dlls[i].name) + 1;

    int idata_size = names_off + names_size;
    unsigned char* idata = cc_alloc(idata_size);

    int desc = 0, slot = 0, name_at = names_off;
    for (int i = 0; i < ndlls; i++)
    {
        int first = 1;
        for (struct import* im = imports; im; im = im->next)
        {
            if (im->dll != &dlls[i])
                continue;
            if (first)
            {
                wr32(idata + desc * 20 + 0, idata_rva + ilt_off + slot * 8);
                wr32(idata + desc * 20 + 16, idata_rva + iat_off + slot * 8);
                first = 0;
            }
            /* hint (0) + name */
            int hint_name = idata_rva + name_at;
            strcpy((char*)idata + name_at + 2, im->import_name);
            name_at += align_up(2 + (int)strlen(im->import_name) + 1, 2);

            wr64(idata + ilt_off + slot * 8, (unsigned)hint_name);
            wr64(idata + iat_off + slot * 8, (unsigned)hint_name);
            im->iat_rva = idata_rva + iat_off + slot * 8;
            slot++;
        }
        if (!first)
        {
            strcpy((char*)idata + name_at, dlls[i].name);
            wr32(idata + desc * 20 + 12, idata_rva + name_at);
            name_at += (int)strlen(dlls[i].name) + 1;
            slot++;     /* zero entry that ends this DLL's tables */
            desc++;
        }
    }

    /* 5. thunks point to their IAT entries */
    for (struct import* im = imports; im; im = im->next)
    {
        int next = text_rva + im->thunk_offset + 6;
        wr32(text->data + im->thunk_offset + 2, (unsigned)(im->iat_rva - next));
    }

    /*
     * 6. .rdata: unwind information, and the debug directory with -g.
     *
     * .pdata has one RUNTIME_FUNCTION per function (begin, end, unwind
     * info). Debuggers (and exceptions) use it to walk the stack on x64.
     * Every function has the same prologue, so they share one UNWIND_INFO:
     *     +0 push rbp        UWOP_PUSH_NONVOL rbp     (code at offset 1)
     *     +1 mov rbp, rsp    UWOP_SET_FPREG           (code at offset 4)
     * The codes are listed from the last to the first.
     */
    int has_debug = obj->sections[SEC_DEBUG_S].size > 0;
    int rdata_rva = align_up(idata_rva + idata_size, SECTION_ALIGN);
    int nfuncs = 0;
    for (struct func_range* fr = obj->funcs; fr; fr = fr->next)
        nfuncs++;
    int pdata_off = 0;
    int xdata_off = 12 * nfuncs;
    int debug_dir_off = xdata_off + 16;
    int rsds_off = debug_dir_off + 28;

    char pdb_path[1024];
    snprintf(pdb_path, sizeof pdb_path, "%s", path);
    char* dot = strrchr(pdb_path, '.');
    if (dot && !strpbrk(dot, "/\\")) *dot = 0;
    strncat(pdb_path, ".pdb", sizeof pdb_path - strlen(pdb_path) - 1);

    int rdata_size = has_debug ? rsds_off + 24 + (int)strlen(pdb_path) + 1 : debug_dir_off;
    unsigned char* rdata = cc_alloc(rdata_size);
    int k = 0;
    for (struct func_range* fr = obj->funcs; fr; fr = fr->next, k++)
    {
        wr32(rdata + 12 * k, (unsigned)(text_rva + fr->start));
        wr32(rdata + 12 * k + 4, (unsigned)(text_rva + fr->end));
        /* the startup code has its own prologue: sub rsp, 56 */
        int startup = strcmp(fr->name, "__cc89_start") == 0;
        wr32(rdata + 12 * k + 8, (unsigned)(rdata_rva + xdata_off + (startup ? 8 : 0)));
    }
    static const unsigned char unwind_info[8] = {
        0x01,           /* version 1, no flags */
        4,              /* size of the prologue */
        2,              /* number of unwind codes */
        0x05,           /* frame register rbp (5), frame offset 0 */
        4, 0x03,        /* at +4: UWOP_SET_FPREG */
        1, 0x50,        /* at +1: UWOP_PUSH_NONVOL, register 5 (rbp) */
    };
    memcpy(rdata + xdata_off, unwind_info, 8);
    static const unsigned char unwind_startup[8] = {
        0x01,           /* version 1 */
        4,              /* size of the prologue: sub rsp, 56 */
        1,              /* one unwind code (the array has an even size) */
        0x00,           /* no frame register */
        4, 0x62,        /* at +4: UWOP_ALLOC_SMALL, (56 / 8) - 1 = 6 */
        0, 0,
    };
    memcpy(rdata + xdata_off + 8, unwind_startup, 8);

    struct pdb_info pdb = { 0 };
    if (has_debug)
    {
        /* a GUID that changes on every link: time and the path */
        unsigned long long seed = (unsigned long long)time(NULL);
        for (const char* c = path; *c; c++)
            seed = seed * 1099511628211ull ^ (unsigned char)*c;
        for (int i = 0; i < 16; i++)
        {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            pdb.guid[i] = (unsigned char)(seed >> 56);
        }
        pdb.age = 1;
        pdb.path = pdb_path;

        /* IMAGE_DEBUG_DIRECTORY: a CodeView record that names the .pdb */
        unsigned char* d = rdata + debug_dir_off;
        wr32(d + 12, 2);                                /* type: CODEVIEW */
        wr32(d + 16, (unsigned)(24 + strlen(pdb_path) + 1));
        wr32(d + 20, (unsigned)(rdata_rva + rsds_off)); /* address */
        /* d + 24: file offset, set when the layout is known */

        /* RSDS record: signature, GUID, age, path of the .pdb */
        unsigned char* r = rdata + rsds_off;
        memcpy(r, "RSDS", 4);
        memcpy(r + 4, pdb.guid, 16);
        wr32(r + 20, pdb.age);
        strcpy((char*)r + 24, pdb_path);
    }

    /* 7. fixups */
    int section_rva[SEC_COUNT] = { text_rva, data_rva };
    for (struct fixup* f = obj->fixups; f; f = f->next)
    {
        struct symbol* s = find_symbol(obj, f->name, 0);
        long long target = section_rva[s->section] + s->offset + f->addend;
        unsigned char* at = obj->sections[f->section].data + f->offset;
        if (f->kind == FIX_REL32)
            wr32(at, (unsigned)(target - (section_rva[f->section] + f->end)));
        else if (f->kind == FIX_SECREL32)
            wr32(at, (unsigned)(s->offset + f->addend));       /* offset inside its section */
        else if (f->kind == FIX_SECIDX)
            wr16(at, (unsigned)(s->section + 1));              /* .text is 1, .data is 2 */
        else
            wr64(at, IMAGE_BASE + (unsigned long long)target);
    }

    struct symbol* entry = find_symbol(obj, "__cc89_start", 0);
    if (entry == NULL || !entry->defined)
        cc_error(path, 0, 0, "internal error: no entry point");

    /* 8. headers */
    int text_raw = align_up(text->size, FILE_ALIGN);
    int data_raw = align_up(data->size, FILE_ALIGN);
    int idata_raw = align_up(idata_size, FILE_ALIGN);
    int rdata_raw = align_up(rdata_size, FILE_ALIGN);
    int image_size = align_up(rdata_rva + rdata_size, SECTION_ALIGN);

    unsigned char h[HEADERS_SIZE] = { 0 };
    h[0] = 'M'; h[1] = 'Z';
    wr32(h + 0x3C, 0x40);                       /* e_lfanew: PE header offset */

    unsigned char* pe = h + 0x40;
    memcpy(pe, "PE\0\0", 4);
    unsigned char* coff = pe + 4;
    wr16(coff + 0, 0x8664);                     /* machine: x64 */
    wr16(coff + 2, 4);                          /* number of sections */
    wr32(coff + 4, (unsigned)time(NULL));       /* time stamp */
    wr16(coff + 16, 240);                       /* size of optional header */
    wr16(coff + 18, 0x0022);                    /* executable, large address aware */

    unsigned char* opt = coff + 20;
    wr16(opt + 0, 0x20B);                       /* PE32+ */
    wr32(opt + 4, text_raw);                    /* size of code */
    wr32(opt + 8, data_raw + idata_raw + rdata_raw);   /* size of initialized data */
    wr32(opt + 16, text_rva + entry->offset);   /* entry point */
    wr32(opt + 20, text_rva);                   /* base of code */
    wr64(opt + 24, IMAGE_BASE);
    wr32(opt + 32, SECTION_ALIGN);
    wr32(opt + 36, FILE_ALIGN);
    wr16(opt + 40, 6);                          /* OS version 6.0 */
    wr16(opt + 48, 6);                          /* subsystem version 6.0 */
    wr32(opt + 56, image_size);
    wr32(opt + 60, HEADERS_SIZE);
    wr16(opt + 68, 3);                          /* subsystem: console */
    wr16(opt + 70, 0x8100);                     /* NX compatible, terminal server aware */
    wr64(opt + 72, 0x100000);                   /* stack reserve */
    wr64(opt + 80, 0x1000);                     /* stack commit */
    wr64(opt + 88, 0x100000);                   /* heap reserve */
    wr64(opt + 96, 0x1000);                     /* heap commit */
    wr32(opt + 108, 16);                        /* number of data directories */
    wr32(opt + 112 + 8 * 1, idata_rva);         /* [1] import directory */
    wr32(opt + 112 + 8 * 1 + 4, desc_size);
    wr32(opt + 112 + 8 * 3, rdata_rva + pdata_off);  /* [3] exception directory (.pdata) */
    wr32(opt + 112 + 8 * 3 + 4, 12 * nfuncs);
    if (has_debug)
    {
        wr32(opt + 112 + 8 * 6, rdata_rva + debug_dir_off);  /* [6] debug directory */
        wr32(opt + 112 + 8 * 6 + 4, 28);
    }
    wr32(opt + 112 + 8 * 12, idata_rva + iat_off);   /* [12] IAT */
    wr32(opt + 112 + 8 * 12 + 4, table_size);

    struct
    {
        const char* name;
        int vsize, rva, raw_size, flags;
    } secs[4] = {
        { ".text", text->size, text_rva, text_raw, 0x60000020 },     /* code, execute, read */
        { ".data", data->size, data_rva, data_raw, 0xC0000040 },     /* data, read, write */
        { ".idata", idata_size, idata_rva, idata_raw, 0xC0000040 },
        { ".rdata", rdata_size, rdata_rva, rdata_raw, 0x40000040 },  /* data, read only */
    };
    unsigned char* sh = opt + 240;
    int raw = HEADERS_SIZE;
    for (int i = 0; i < 4; i++, sh += 40)
    {
        memcpy(sh, secs[i].name, strlen(secs[i].name));
        wr32(sh + 8, secs[i].vsize ? secs[i].vsize : 1);
        wr32(sh + 12, secs[i].rva);
        wr32(sh + 16, secs[i].raw_size);
        wr32(sh + 20, secs[i].raw_size ? raw : 0);
        wr32(sh + 36, secs[i].flags);
        if (i == 3 && has_debug)
            wr32(rdata + debug_dir_off + 24, (unsigned)(raw + rsds_off));   /* file offset of RSDS */
        raw += secs[i].raw_size;
    }

    /* 9. write the file */
    FILE* f = fopen(path, "wb");
    if (f == NULL)
        cc_error(path, 0, 0, "cannot write");
    fwrite(h, 1, HEADERS_SIZE, f);
    const unsigned char* contents[4] = { text->data, data->data, idata, rdata };
    int sizes[4] = { text->size, data->size, idata_size, rdata_size };
    for (int i = 0; i < 4; i++)
    {
        if (sizes[i])
            fwrite(contents[i], 1, sizes[i], f);
        for (int pad = secs[i].raw_size - sizes[i]; pad > 0; pad--)
            fputc(0, f);
    }
    fclose(f);

    /* 10. -g: the .pdb, which also needs the section headers */
    if (has_debug)
    {
        pdb.section_headers = opt + 240;
        pdb.nsections = 4;
        write_pdb(obj, &pdb);
    }
}
