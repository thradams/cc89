/*
 * Linker, macOS arm64: object + dylibs -> Mach-O executable.
 *
 * Memory layout (pages of 16 KB):
 *
 *     __PAGEZERO   0 .. 4 GB, no access (catches null pointers)
 *     __TEXT       0x100000000: Mach-O header, load commands, __text
 *                  (the code, then one stub per imported function)
 *     __DATA       __data (all static objects), __got (imports)
 *     __LINKEDIT   chained fixups, export trie, symbol table, signature
 *
 * Debug information (-g). The executable carries only LC_UUID; the DWARF
 * goes into a separate file, the way dsymutil does it:
 *     out.dSYM/Contents/Resources/DWARF/out
 * an MH_DSYM with the same UUID and a __DWARF segment (__debug_info,
 * __debug_abbrev, __debug_line). lldb finds it next to the executable.
 *
 * There is no startup code: LC_MAIN gives the offset of main, and dyld
 * calls main(argc, argv, envp, apple) and then exit with its result.
 *
 * Imports. Every function of a library is called through a stub
 *         adrp x16, got@page
 *         ldr  x16, [x16, got@pageoff]
 *         br   x16
 * and every import has one GOT entry that dyld fills when it loads the
 * program. C names take a '_' in front: printf is "_printf".
 *
 * Fixups. The executable is position independent (MH_PIE), so every
 * address stored in __DATA (pointers in static objects and the GOT)
 * must be fixed by dyld. With LC_DYLD_CHAINED_FIXUPS the information
 * lives inside the pointers themselves (DYLD_CHAINED_PTR_64_OFFSET):
 *
 *     rebase: bit 63 = 0, bits 51-62 next, bits 0-35 target (offset from the header)
 *     bind:   bit 63 = 1, bits 51-62 next, bits 24-31 addend, bits 0-23 import
 *
 * "next" is the distance to the next pointer of the same page, in units
 * of 4 bytes (0 = last). The table in __LINKEDIT gives the first pointer
 * of each page and the list of imported names.
 *
 * Code signature. arm64 macOS does not run unsigned code. An ad-hoc
 * signature is a CodeDirectory with the SHA-256 of every 4 KB page of
 * the file before it; no certificate is needed.
 */
#include "object.h"
#include "cc89.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir(p, 0755)
#endif

#define PAGE        16384
#define BASE        0x100000000ull      /* __TEXT starts after the 4 GB of __PAGEZERO */
#define SIGN_PAGE   4096

#define LC_SEGMENT_64           0x19
#define LC_SYMTAB               0x2
#define LC_DYSYMTAB             0xB
#define LC_LOAD_DYLIB           0xC
#define LC_LOAD_DYLINKER        0xE
#define LC_UUID                 0x1B
#define LC_CODE_SIGNATURE       0x1D
#define LC_BUILD_VERSION        0x32
#define LC_MAIN                 0x80000028
#define LC_DYLD_EXPORTS_TRIE    0x80000033
#define LC_DYLD_CHAINED_FIXUPS  0x80000034

#define DYLD_CHAINED_PTR_64_OFFSET  6
#define DYLD_CHAINED_PTR_START_NONE 0xFFFF
#define FLAT_LOOKUP                 0xFE    /* lib ordinal -2: search every library */

static const char dylinker[] = "/usr/lib/dyld";
static const char libsystem[] = "/usr/lib/libSystem.B.dylib";

struct buf
{
    unsigned char* data;
    int size;
    int capacity;
};

static void put(struct buf* b, const void* p, int n)
{
    if (b->size + n > b->capacity)
    {
        b->capacity = (b->size + n) * 2 + 64;
        b->data = realloc(b->data, b->capacity);
        if (b->data == NULL)
        {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
    }
    if (p) memcpy(b->data + b->size, p, n);
    else memset(b->data + b->size, 0, n);
    b->size += n;
}

static void w16(unsigned char* p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void w32(unsigned char* p, unsigned v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i)); }
static void w64(unsigned char* p, unsigned long long v) { for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i)); }
static void put32(struct buf* b, unsigned v) { unsigned char x[4]; w32(x, v); put(b, x, 4); }
static void put64(struct buf* b, unsigned long long v) { unsigned char x[8]; w64(x, v); put(b, x, 8); }
static void pad(struct buf* b, int align) { while (b->size % align) put(b, "", 1); }

/* the code signature is big endian */
static void be32(unsigned char* p, unsigned v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (24 - 8 * i)); }
static void be64(unsigned char* p, unsigned long long v) { for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (56 - 8 * i)); }

static int align_up(int n, int a)
{
    return (n + a - 1) / a * a;
}

static void section_put(struct section* s, int byte)
{
    if (s->size + 1 > s->capacity)
    {
        s->capacity = s->capacity ? s->capacity * 2 : 4096;
        s->data = realloc(s->data, s->capacity);
        if (s->data == NULL)
        {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
    }
    s->data[s->size++] = (unsigned char)byte;
}

static void section_put32(struct section* s, unsigned v)
{
    for (int i = 0; i < 4; i++)
        section_put(s, (int)(v >> (8 * i)) & 0xFF);
}

/* ---------------------------------------------------------------- SHA-256 */

static const unsigned sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))

static void sha256_block(unsigned h[8], const unsigned char* p)
{
    unsigned w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (unsigned)p[4 * i] << 24 | (unsigned)p[4 * i + 1] << 16 | (unsigned)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++)
    {
        unsigned s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        unsigned s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    unsigned a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++)
    {
        unsigned t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
        unsigned t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

static void sha256(const unsigned char* data, int n, unsigned char out[32])
{
    unsigned h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    int i = 0;
    for (; i + 64 <= n; i += 64)
        sha256_block(h, data + i);
    unsigned char last[128] = { 0 };
    int rest = n - i;
    memcpy(last, data + i, rest);
    last[rest] = 0x80;
    int blocks = rest + 9 > 64 ? 2 : 1;
    be64(last + 64 * blocks - 8, (unsigned long long)n * 8);
    for (int b = 0; b < blocks; b++)
        sha256_block(h, last + 64 * b);
    for (int j = 0; j < 8; j++)
        be32(out + 4 * j, h[j]);
}

/* ------------------------------------------------------------------ linker */

struct import
{
    struct import* next;
    struct symbol* sym;
    int index;          /* in the chained fixups import table */
    int got;            /* GOT entry */
    int stub;           /* offset of the stub in .text, -1 for data */
};

/* one pointer of __DATA that dyld fixes */
struct chained
{
    int offset;                 /* in the __DATA segment */
    unsigned long long value;   /* rebase or bind, without "next" */
};

static int compare_chained(const void* a, const void* b)
{
    return ((const struct chained*)a)->offset - ((const struct chained*)b)->offset;
}

static struct import* find_import(struct import* list, struct symbol* s)
{
    for (struct import* im = list; im; im = im->next)
        if (im->sym == s) return im;
    return NULL;
}

static void put_segment(struct buf* b, const char* name, unsigned long long vmaddr, unsigned long long vmsize,
                        unsigned long long fileoff, unsigned long long filesize, int prot, int nsects)
{
    unsigned char name16[16] = { 0 };
    memcpy(name16, name, strlen(name));
    put32(b, LC_SEGMENT_64);
    put32(b, 72 + 80 * nsects);
    put(b, name16, 16);
    put64(b, vmaddr);
    put64(b, vmsize);
    put64(b, fileoff);
    put64(b, filesize);
    put32(b, prot);     /* maxprot */
    put32(b, prot);     /* initprot */
    put32(b, nsects);
    put32(b, 0);
}

static void put_section(struct buf* b, const char* sect, const char* seg, unsigned long long addr,
                        unsigned long long size, int offset, int align_log2, unsigned flags)
{
    unsigned char name16[16] = { 0 };
    memcpy(name16, sect, strlen(sect));
    put(b, name16, 16);
    memset(name16, 0, 16);
    memcpy(name16, seg, strlen(seg));
    put(b, name16, 16);
    put64(b, addr);
    put64(b, size);
    put32(b, offset);
    put32(b, align_log2);
    put32(b, 0);        /* reloff */
    put32(b, 0);        /* nreloc */
    put32(b, flags);
    put32(b, 0);
    put32(b, 0);
    put32(b, 0);
}

static void put_linkedit_data(struct buf* b, unsigned cmd, int off, int size)
{
    put32(b, cmd);
    put32(b, 16);
    put32(b, off);
    put32(b, size);
}

static void put_dylib(struct buf* b, unsigned cmd, const char* name)
{
    int n = (int)strlen(name) + 1;
    int size = align_up(24 + n, 8);
    put32(b, cmd);
    put32(b, size);
    put32(b, 24);           /* name offset */
    put32(b, 2);            /* timestamp */
    put32(b, 0x10000);      /* current version 1.0.0 */
    put32(b, 0x10000);      /* compatibility version */
    put(b, name, n);
    put(b, NULL, size - 24 - n);
}

/* ------------------------------------------------------------------- dSYM */

struct dsym_layout
{
    unsigned long long text_seg_size, text_va, text_size;
    unsigned long long data_seg_off, data_seg_size, data_va, data_size;
    unsigned long long linkedit_va, linkedit_size;
    unsigned long long dwarf_va;    /* after __LINKEDIT */
};

/* path + ".dSYM/Contents/Resources/DWARF/" + name of the executable */
static void write_dsym(struct object* obj, const char* path, const unsigned char uuid[16], const struct dsym_layout* l)
{
    unsigned char* line_data;
    int line_size;
    dwarf_line_table(obj, l->text_va, &line_data, &line_size);
    struct section* info = &obj->sections[SEC_DEBUG_INFO];
    struct section* abbrev = &obj->sections[SEC_DEBUG_ABBREV];

    /* __debug_aranges: lldb finds the compile unit of an address here */
    unsigned long long code_end = l->text_va;
    for (struct func_range* fr = obj->funcs; fr; fr = fr->next)
        if (l->text_va + fr->end > code_end) code_end = l->text_va + fr->end;
    struct buf aranges = { 0 };
    put32(&aranges, 44);                 /* unit length */
    put(&aranges, "\x02\x00", 2);        /* version 2 */
    put32(&aranges, 0);                  /* offset of the unit in __debug_info */
    put(&aranges, "\x08\x00", 2);        /* address size, segment size */
    put32(&aranges, 0);                  /* pad to 16 */
    put64(&aranges, l->text_va);
    put64(&aranges, code_end - l->text_va);
    put64(&aranges, 0);
    put64(&aranges, 0);

    const char* name = path;
    for (const char* p = path; *p; p++)
        if (*p == '/' || *p == '\\') name = p + 1;
    int n = (int)strlen(path);
    char* dir = cc_alloc(n + 64);
    char* file = cc_alloc(n + 64 + (int)strlen(name));
    static const char* const parts[] = { ".dSYM", "/Contents", "/Resources", "/DWARF" };
    strcpy(dir, path);
    for (int i = 0; i < 4; i++)
    {
        strcat(dir, parts[i]);
        make_dir(dir);
    }
    sprintf(file, "%s/%s", dir, name);

    /* load commands: UUID, empty symtab, the segments of the executable
       (same names and addresses, no contents, as dsymutil writes them;
       lldb merges them with the executable's) and __DWARF */
    int sizeofcmds = 24 + 24 + 72 + (72 + 80) + (72 + 80) + 72 + (72 + 80 * 4);
    int line_off = align_up(32 + sizeofcmds, 16);
    int info_off = line_off + line_size;
    int abbrev_off = info_off + info->size;
    int aranges_off = abbrev_off + abbrev->size;
    int file_size = aranges_off + aranges.size;
    unsigned long long dwarf_size = (unsigned long long)(file_size - line_off);

    struct buf cmds = { 0 };
    put32(&cmds, LC_UUID);
    put32(&cmds, 24);
    put(&cmds, uuid, 16);
    put32(&cmds, LC_SYMTAB);
    put32(&cmds, 24);
    put32(&cmds, 0);
    put32(&cmds, 0);
    put32(&cmds, 0);
    put32(&cmds, 0);
    put_segment(&cmds, "__PAGEZERO", 0, BASE, 0, 0, 0, 0);
    put_segment(&cmds, "__TEXT", BASE, l->text_seg_size, 0, 0, 5, 1);
    put_section(&cmds, "__text", "__TEXT", l->text_va, l->text_size, 0, 2, 0x80000400);
    put_segment(&cmds, "__DATA", BASE + l->data_seg_off, l->data_seg_size, 0, 0, 3, 1);
    put_section(&cmds, "__data", "__DATA", l->data_va, l->data_size, 0, 4, 0);
    put_segment(&cmds, "__LINKEDIT", l->linkedit_va, l->linkedit_size, 0, 0, 1, 0);
    put_segment(&cmds, "__DWARF", l->dwarf_va, align_up((int)dwarf_size, PAGE), line_off, dwarf_size, 7, 4);
    put_section(&cmds, "__debug_line", "__DWARF", l->dwarf_va, line_size, line_off, 0, 0);
    put_section(&cmds, "__debug_info", "__DWARF", l->dwarf_va + (info_off - line_off), info->size, info_off, 0, 0);
    put_section(&cmds, "__debug_abbrev", "__DWARF", l->dwarf_va + (abbrev_off - line_off), abbrev->size, abbrev_off, 0, 0);
    put_section(&cmds, "__debug_aranges", "__DWARF", l->dwarf_va + (aranges_off - line_off), aranges.size, aranges_off, 0, 0);
    if (cmds.size != sizeofcmds)
        cc_error(file, 0, 0, "internal error: dSYM load commands %d != %d", cmds.size, sizeofcmds);

    unsigned char* o = cc_alloc(file_size);
    w32(o, 0xFEEDFACF);                 /* MH_MAGIC_64 */
    w32(o + 4, 0x0100000C);             /* CPU_TYPE_ARM64 */
    w32(o + 8, 0);
    w32(o + 12, 0xA);                   /* MH_DSYM */
    w32(o + 16, 7);
    w32(o + 20, sizeofcmds);
    memcpy(o + 32, cmds.data, cmds.size);
    memcpy(o + line_off, line_data, line_size);
    memcpy(o + info_off, info->data, info->size);
    memcpy(o + abbrev_off, abbrev->data, abbrev->size);
    memcpy(o + aranges_off, aranges.data, aranges.size);

    FILE* f = fopen(file, "wb");
    if (f == NULL)
        cc_error(file, 0, 0, "cannot write");
    fwrite(o, 1, file_size, f);
    fclose(f);
    free(o);
    free(cmds.data);
    free(line_data);
    free(aranges.data);
}

void link_macho(struct object* obj, const char** libs, int nlibs, const char* path, const char* map_path)
{
    struct section* text = &obj->sections[SEC_TEXT];
    struct section* data = &obj->sections[SEC_DATA];

    struct symbol* main_sym = find_symbol(obj, "main", 0);
    if (main_sym == NULL || !main_sym->defined || main_sym->section != SEC_TEXT)
        cc_error(path, 0, 0, "undefined symbol 'main'");

    /* 1. imports: every name used but not defined; GOT entries */
    struct import* imports = NULL;
    struct import** tail = &imports;
    int nimports = 0, ngot = 0;
    struct import* got_local = NULL;    /* GOT entries of symbols defined here */
    for (struct fixup* f = obj->fixups; f; f = f->next)
    {
        struct symbol* s = find_symbol(obj, f->name, 1);
        int via_got = f->kind == FIX_A64_GOT_PAGE21 || f->kind == FIX_A64_GOT_LO12;
        if (s->defined)
        {
            if (via_got && find_import(got_local, s) == NULL)
            {
                struct import* im = cc_alloc(sizeof *im);
                im->sym = s;
                im->got = ngot++;
                im->stub = -1;
                im->next = got_local;
                got_local = im;
            }
            continue;
        }
        if (find_import(imports, s))
            continue;
        struct import* im = cc_alloc(sizeof *im);
        im->sym = s;
        im->index = nimports++;
        im->got = ngot++;
        im->stub = -1;
        *tail = im;
        tail = &im->next;
    }

    /* 2. stubs for imported functions, at the end of .text */
    while (text->size % 4)
        section_put(text, 0);
    for (struct import* im = imports; im; im = im->next)
    {
        if (im->sym->is_data)
            continue;
        im->stub = text->size;
        section_put32(text, 0x90000010);    /* adrp x16, got@page */
        section_put32(text, 0xF9400210);    /* ldr  x16, [x16, got@pageoff] */
        section_put32(text, 0xD61F0200);    /* br   x16 */
    }

    /* never empty, so __DATA always has something */
    if (data->size + 8 * ngot == 0)
        for (int i = 0; i < 8; i++) section_put(data, 0);

    /* 3. load commands (their size decides where the code starts) */
    int nsegments = 4;
    int ncmds = 0;
    int sizeofcmds = 0;
    {
        int dylinker_size = align_up(12 + (int)sizeof dylinker, 8);
        sizeofcmds += 72;                   /* __PAGEZERO */
        sizeofcmds += 72 + 80;              /* __TEXT */
        sizeofcmds += 72 + 80 * 2;          /* __DATA */
        sizeofcmds += 72;                   /* __LINKEDIT */
        sizeofcmds += 16 + 16;              /* chained fixups, exports trie */
        sizeofcmds += 24 + 80;              /* symtab, dysymtab */
        sizeofcmds += dylinker_size;
        sizeofcmds += 24;                   /* build version */
        sizeofcmds += 24;                   /* main */
        sizeofcmds += align_up(24 + (int)sizeof libsystem, 8);
        for (int i = 0; i < nlibs; i++)
            sizeofcmds += align_up(24 + (int)strlen(libs[i]) + 1, 8);
        sizeofcmds += 16;                   /* code signature */
        sizeofcmds += 24;                   /* uuid */
        ncmds = 14 + nlibs;
    }

    /* 4. layout */
    int text_off = align_up(32 + sizeofcmds, 16);
    int text_seg_size = align_up(text_off + text->size, PAGE);
    int data_seg_off = text_seg_size;
    int data_off = data_seg_off;
    int got_off = align_up(data_off + data->size, 8);
    int data_end = got_off + 8 * ngot;
    int data_seg_size = align_up(data_end - data_seg_off, PAGE);
    int linkedit_off = data_seg_off + data_seg_size;

    unsigned long long text_va = BASE + text_off;
    unsigned long long data_va = BASE + data_off;
    unsigned long long got_va = BASE + got_off;

    #define SYM_VA(s) ((s)->section == SEC_TEXT ? text_va + (s)->offset : data_va + (s)->offset)

    /* 5. stubs point to their GOT entries */
    for (struct import* im = imports; im; im = im->next)
    {
        if (im->stub < 0)
            continue;
        unsigned long long P = text_va + im->stub;
        unsigned long long G = got_va + 8 * im->got;
        a64_patch(text->data + im->stub, FIX_A64_PAGE21, G, P);
        a64_patch(text->data + im->stub + 4, FIX_A64_GOT_LO12, G, P + 4);
    }

    /* 6. fixups; the pointers of __DATA become chained fixups */
    int nchained_max = ngot;
    for (struct fixup* f = obj->fixups; f; f = f->next)
        nchained_max++;
    struct chained* chain = cc_alloc(sizeof(struct chained) * (nchained_max + 1));
    int nchain = 0;

    for (struct import* im = got_local; im; im = im->next)
    {
        chain[nchain].offset = got_off - data_seg_off + 8 * im->got;
        chain[nchain].value = SYM_VA(im->sym) - BASE;
        nchain++;
    }
    for (struct import* im = imports; im; im = im->next)
    {
        chain[nchain].offset = got_off - data_seg_off + 8 * im->got;
        chain[nchain].value = 1ull << 63 | (unsigned long long)im->index;
        nchain++;
    }

    for (struct fixup* f = obj->fixups; f; f = f->next)
    {
        struct symbol* s = find_symbol(obj, f->name, 0);
        unsigned long long base = f->section == SEC_TEXT ? text_va : data_va;
        unsigned long long P = base + f->offset;
        unsigned char* at = obj->sections[f->section].data + f->offset;
        struct import* im = s->defined ? find_import(got_local, s) : find_import(imports, s);

        if (f->section == SEC_DEBUG_INFO)
        {
            /* addresses in the DWARF are fixed: the dSYM is not loaded */
            w64(at, SYM_VA(s) + f->addend);
            continue;
        }

        if (f->kind == FIX_A64_GOT_PAGE21 || f->kind == FIX_A64_GOT_LO12)
        {
            a64_patch(at, f->kind, got_va + 8 * im->got, P);
            continue;
        }

        if (f->kind == FIX_ABS64)
        {
            if (f->section != SEC_DATA)
                cc_error(path, 0, 0, "absolute address of '%s' in code", f->name);
            if (f->offset % 4)
                cc_error(path, 0, 0, "address of '%s' at an unaligned place", f->name);
            chain[nchain].offset = data_off - data_seg_off + f->offset;
            if (s->defined)
            {
                chain[nchain].value = SYM_VA(s) + f->addend - BASE;
            }
            else
            {
                if (f->addend < 0 || f->addend > 255)
                    cc_error(path, 0, 0, "address of '%s%+lld' not supported", f->name, f->addend);
                chain[nchain].value = 1ull << 63 | (unsigned long long)f->addend << 24 | (unsigned long long)im->index;
            }
            w64(at, 0);
            nchain++;
            continue;
        }

        unsigned long long target = 0;
        if (s->defined)
            target = SYM_VA(s);
        else if (im->stub >= 0)
            target = text_va + im->stub;    /* a function of a library: its stub */
        else
            cc_error(path, 0, 0, "'%s' of a library used without its GOT entry", f->name);
        a64_patch(at, f->kind, target + f->addend, P);
    }

    /* 7. __DATA contents, with the chains: each pointer gives the distance to the next one of its page */
    unsigned char* data_seg = cc_alloc(data_seg_size);
    memcpy(data_seg + (data_off - data_seg_off), data->data, data->size);
    qsort(chain, nchain, sizeof *chain, compare_chained);
    int npages = data_seg_size / PAGE;
    unsigned short* page_start = cc_alloc(sizeof(unsigned short) * npages);
    for (int i = 0; i < npages; i++)
        page_start[i] = DYLD_CHAINED_PTR_START_NONE;
    for (int i = 0; i < nchain; i++)
    {
        int page = chain[i].offset / PAGE;
        if (page_start[page] == DYLD_CHAINED_PTR_START_NONE)
            page_start[page] = (unsigned short)(chain[i].offset % PAGE);
        unsigned long long next = 0;
        if (i + 1 < nchain && chain[i + 1].offset / PAGE == page)
            next = (unsigned long long)(chain[i + 1].offset - chain[i].offset) / 4;
        w64(data_seg + chain[i].offset, chain[i].value | next << 51);
    }

    /* 8. __LINKEDIT: chained fixups table */
    int flat = nlibs > 0;
    struct buf linkedit = { 0 };
    int fixups_off = linkedit.size;
    {
        struct buf names = { 0 };
        put(&names, "", 1);
        struct buf imp = { 0 };
        for (struct import* im = imports; im; im = im->next)
        {
            unsigned ordinal = flat ? FLAT_LOOKUP : 1;
            put32(&imp, ordinal | (unsigned)names.size << 9);
            put(&names, "_", 1);
            put(&names, im->sym->name, (int)strlen(im->sym->name) + 1);
        }

        int starts_offset = 32;
        int seg_info = 4 + 4 * nsegments;
        seg_info = align_up(seg_info, 8);
        int seg_size = 22 + 2 * npages;
        int imports_offset = align_up(starts_offset + seg_info + seg_size, 4);
        int symbols_offset = imports_offset + imp.size;

        put32(&linkedit, 0);                 /* fixups_version */
        put32(&linkedit, starts_offset);
        put32(&linkedit, imports_offset);
        put32(&linkedit, symbols_offset);
        put32(&linkedit, nimports);
        put32(&linkedit, 1);                 /* DYLD_CHAINED_IMPORT */
        put32(&linkedit, 0);                 /* names not compressed */
        pad(&linkedit, 8);

        /* starts in image: one entry per segment, only __DATA has pointers */
        put32(&linkedit, nsegments);
        put32(&linkedit, 0);
        put32(&linkedit, 0);
        put32(&linkedit, nchain ? seg_info : 0);
        put32(&linkedit, 0);
        pad(&linkedit, 8);

        unsigned char s[22];
        w32(s, seg_size);
        w16(s + 4, PAGE);
        w16(s + 6, DYLD_CHAINED_PTR_64_OFFSET);
        w64(s + 8, (unsigned long long)data_seg_off);   /* offset from the header in memory = in the file */
        w32(s + 16, 0);                                 /* max_valid_pointer: only for 32 bits */
        w16(s + 20, npages);
        put(&linkedit, s, 22);
        for (int i = 0; i < npages; i++)
        {
            unsigned char x[2];
            w16(x, page_start[i]);
            put(&linkedit, x, 2);
        }
        pad(&linkedit, 4);
        put(&linkedit, imp.data, imp.size);
        put(&linkedit, names.data, names.size);
        free(names.data);
        free(imp.data);
    }
    int fixups_size = linkedit.size - fixups_off;
    pad(&linkedit, 8);

    /* export trie: one node, not terminal, no children (the program exports nothing) */
    int trie_off = linkedit.size;
    put(&linkedit, NULL, 2);
    int trie_size = 2;
    pad(&linkedit, 8);

    /* symbol table: the functions and objects defined here, for debuggers and crash reports */
    struct buf strtab = { 0 };
    put(&strtab, " ", 2);
    int symtab_off = linkedit.size;
    int nsyms = 0;
    for (int b = 0; b < SYMBOL_BUCKETS; b++)
    {
        for (struct symbol* s = obj->buckets[b]; s; s = s->next)
        {
            if (!s->defined || s->name[0] == '.')
                continue;
            unsigned char nl[16] = { 0 };
            w32(nl, strtab.size);
            nl[4] = 0x0E;                                   /* N_SECT */
            nl[5] = s->section == SEC_TEXT ? 1 : 2;         /* __text, __data */
            w64(nl + 8, SYM_VA(s));
            put(&linkedit, nl, 16);
            put(&strtab, "_", 1);
            put(&strtab, s->name, (int)strlen(s->name) + 1);
            nsyms++;
        }
    }
    int strtab_off = linkedit.size;
    pad(&strtab, 8);
    put(&linkedit, strtab.data, strtab.size);
    pad(&linkedit, 16);

    /* code signature: size known now, contents after the rest of the file is written */
    const char* ident = path;
    for (const char* p = path; *p; p++)
        if (*p == '/' || *p == '\\') ident = p + 1;
    int ident_size = (int)strlen(ident) + 1;
    int sig_off = linkedit_off + linkedit.size;
    int nslots = (sig_off + SIGN_PAGE - 1) / SIGN_PAGE;
    int cd_size = 88 + ident_size + 32 * nslots;
    int sig_size = align_up(12 + 8 + cd_size, 16);
    int sig_local = linkedit.size;
    put(&linkedit, NULL, sig_size);
    int linkedit_size = linkedit.size;

    /* 9. header and load commands */
    struct buf cmds = { 0 };
    put_segment(&cmds, "__PAGEZERO", 0, BASE, 0, 0, 0, 0);
    put_segment(&cmds, "__TEXT", BASE, text_seg_size, 0, text_seg_size, 5, 1);
    put_section(&cmds, "__text", "__TEXT", text_va, text->size, text_off, 2, 0x80000400);
    put_segment(&cmds, "__DATA", BASE + data_seg_off, data_seg_size, data_seg_off, data_seg_size, 3, 2);
    put_section(&cmds, "__data", "__DATA", data_va, data->size, data_off, 4, 0);
    put_section(&cmds, "__got", "__DATA", got_va, 8 * ngot, got_off, 3, 0);
    put_segment(&cmds, "__LINKEDIT", BASE + linkedit_off, align_up(linkedit_size, PAGE), linkedit_off, linkedit_size, 1, 0);
    put_linkedit_data(&cmds, LC_DYLD_CHAINED_FIXUPS, linkedit_off + fixups_off, fixups_size);
    put_linkedit_data(&cmds, LC_DYLD_EXPORTS_TRIE, linkedit_off + trie_off, trie_size);

    put32(&cmds, LC_SYMTAB);
    put32(&cmds, 24);
    put32(&cmds, linkedit_off + symtab_off);
    put32(&cmds, nsyms);
    put32(&cmds, linkedit_off + strtab_off);
    put32(&cmds, strtab.size);

    put32(&cmds, LC_DYSYMTAB);
    put32(&cmds, 80);
    put32(&cmds, 0);            /* ilocalsym */
    put32(&cmds, nsyms);        /* nlocalsym */
    put32(&cmds, nsyms);        /* iextdefsym */
    put32(&cmds, 0);
    put32(&cmds, nsyms);        /* iundefsym */
    put(&cmds, NULL, 80 - 28);

    int dylinker_size = align_up(12 + (int)sizeof dylinker, 8);
    put32(&cmds, LC_LOAD_DYLINKER);
    put32(&cmds, dylinker_size);
    put32(&cmds, 12);
    put(&cmds, dylinker, sizeof dylinker);
    put(&cmds, NULL, dylinker_size - 12 - (int)sizeof dylinker);

    put32(&cmds, LC_BUILD_VERSION);
    put32(&cmds, 24);
    put32(&cmds, 1);            /* PLATFORM_MACOS */
    put32(&cmds, 0x000B0000);   /* minos 11.0 */
    put32(&cmds, 0x000B0000);   /* sdk 11.0 */
    put32(&cmds, 0);            /* no tools */

    put32(&cmds, LC_MAIN);
    put32(&cmds, 24);
    put64(&cmds, (unsigned long long)(text_off + main_sym->offset));
    put64(&cmds, 0);            /* default stack size */

    put_dylib(&cmds, LC_LOAD_DYLIB, libsystem);
    for (int i = 0; i < nlibs; i++)
        put_dylib(&cmds, LC_LOAD_DYLIB, libs[i]);

    put_linkedit_data(&cmds, LC_CODE_SIGNATURE, sig_off, sig_size);

    /* uuid: a hash of the code and data, so the dSYM of another build does not match */
    unsigned char uuid[32];
    {
        struct buf h = { 0 };
        put(&h, text->data, text->size);
        put(&h, data->data, data->size);
        sha256(h.data, h.size, uuid);
        free(h.data);
        uuid[6] = (uuid[6] & 0x0F) | 0x30;   /* RFC 4122, name-based */
        uuid[8] = (uuid[8] & 0x3F) | 0x80;
    }
    put32(&cmds, LC_UUID);
    put32(&cmds, 24);
    put(&cmds, uuid, 16);

    if (cmds.size != sizeofcmds)
        cc_error(path, 0, 0, "internal error: load commands %d != %d", cmds.size, sizeofcmds);

    /* 10. the file */
    int file_size = linkedit_off + linkedit_size;
    unsigned char* o = cc_alloc(file_size);
    w32(o, 0xFEEDFACF);                 /* MH_MAGIC_64 */
    w32(o + 4, 0x0100000C);             /* CPU_TYPE_ARM64 */
    w32(o + 8, 0);                      /* CPU_SUBTYPE_ARM64_ALL */
    w32(o + 12, 2);                     /* MH_EXECUTE */
    w32(o + 16, ncmds);
    w32(o + 20, sizeofcmds);
    w32(o + 24, 0x1 | 0x4 | (flat ? 0 : 0x80) | 0x200000);   /* NOUNDEFS, DYLDLINK, TWOLEVEL, PIE */
    memcpy(o + 32, cmds.data, cmds.size);
    memcpy(o + text_off, text->data, text->size);
    memcpy(o + data_seg_off, data_seg, data_seg_size);
    memcpy(o + linkedit_off, linkedit.data, linkedit.size);

    /* 11. code signature: SuperBlob with one CodeDirectory */
    unsigned char* sig = o + linkedit_off + sig_local;
    be32(sig, 0xFADE0CC0);              /* CSMAGIC_EMBEDDED_SIGNATURE */
    be32(sig + 4, 12 + 8 + cd_size);
    be32(sig + 8, 1);                   /* one blob */
    be32(sig + 12, 0);                  /* CSSLOT_CODEDIRECTORY */
    be32(sig + 16, 20);
    unsigned char* cd = sig + 20;
    be32(cd, 0xFADE0C02);               /* CSMAGIC_CODEDIRECTORY */
    be32(cd + 4, cd_size);
    be32(cd + 8, 0x20400);              /* version with exec segment */
    be32(cd + 12, 0x20002);             /* adhoc | linker signed */
    be32(cd + 16, 88 + ident_size);     /* hashOffset */
    be32(cd + 20, 88);                  /* identOffset */
    be32(cd + 24, 0);                   /* nSpecialSlots */
    be32(cd + 28, nslots);              /* nCodeSlots */
    be32(cd + 32, sig_off);             /* codeLimit */
    cd[36] = 32;                        /* hashSize */
    cd[37] = 2;                         /* CS_HASHTYPE_SHA256 */
    cd[38] = 0;                         /* platform */
    cd[39] = 12;                        /* pageSize: 2^12 */
    /* 40..55: spare2, scatterOffset, teamOffset, spare3 = 0; 56: codeLimit64 = 0 */
    be64(cd + 64, 0);                   /* execSegBase */
    be64(cd + 72, (unsigned long long)text_seg_size);   /* execSegLimit */
    be64(cd + 80, 1);                   /* execSegFlags: CS_EXECSEG_MAIN_BINARY */
    memcpy(cd + 88, ident, ident_size);
    for (int i = 0; i < nslots; i++)
    {
        int start = i * SIGN_PAGE;
        int n = sig_off - start < SIGN_PAGE ? sig_off - start : SIGN_PAGE;
        sha256(o + start, n, cd + 88 + ident_size + 32 * i);
    }

    if (map_path)
    {
        FILE* m = fopen(map_path, "w");
        if (m)
        {
            for (int b = 0; b < SYMBOL_BUCKETS; b++)
                for (struct symbol* s = obj->buckets[b]; s; s = s->next)
                    if (s->defined && s->name[0] != '.')
                        fprintf(m, "%016llx %s\n", SYM_VA(s), s->name);
            fclose(m);
        }
    }

    FILE* f = fopen(path, "wb");
    if (f == NULL)
        cc_error(path, 0, 0, "cannot write");
    fwrite(o, 1, file_size, f);
    fclose(f);
#ifndef _WIN32
    chmod(path, 0755);
#endif

    if (obj->sections[SEC_DEBUG_INFO].size > 0)
    {
        struct dsym_layout l;
        l.text_seg_size = (unsigned long long)text_seg_size;
        l.text_va = text_va;
        l.text_size = (unsigned long long)text->size;
        l.data_seg_off = (unsigned long long)data_seg_off;
        l.data_seg_size = (unsigned long long)data_seg_size;
        l.data_va = data_va;
        l.data_size = (unsigned long long)data->size;
        l.linkedit_va = BASE + linkedit_off;
        l.linkedit_size = (unsigned long long)align_up(linkedit_size, PAGE);
        l.dwarf_va = BASE + linkedit_off + (unsigned long long)align_up(linkedit_size, PAGE);
        write_dsym(obj, path, uuid, &l);
    }
}
