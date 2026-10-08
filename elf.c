/*
 * Linker: object -> Linux x64 executable (ELF64, dynamically linked).
 *
 * The program is linked against shared libraries (libc.so.6 and
 * libm.so.6 by default) without reading them: every name the program
 * uses and does not define is left for the dynamic loader
 * (/lib64/ld-linux-x86-64.so.2), which looks it up when the program
 * starts. A missing name is reported by the loader, not here.
 *
 * The image is not position independent; it always loads at 0x400000:
 *
 *   segment 1, read + execute (file offset 0)
 *       ELF header, program headers, interpreter path,
 *       dynamic symbols, their names, hash table, relocations,
 *       code, then one thunk per imported function:
 *           jmp qword ptr [rip+GOT entry]
 *   segment 2, read + write (next page)
 *       data, GOT (global offset table), dynamic section
 *
 * GOT: one 8-byte entry per imported symbol. The loader writes the
 * address of the symbol there (relocation R_X86_64_GLOB_DAT) before the
 * program runs (DF_BIND_NOW). Code reaches objects of a library, like
 * stdout, through their GOT entry: mov rax, [rip+stdout@GOT].
 */
#include "object.h"
#include "cc89.h"
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#define BASE        0x400000ull
#define PAGE        0x1000
#define NPHDR       6

/* ELF constants used here */
#define PT_LOAD      1
#define PT_DYNAMIC   2
#define PT_INTERP    3
#define PT_PHDR      6
#define PT_GNU_STACK 0x6474e551
#define R_X86_64_64        1
#define R_X86_64_GLOB_DAT  6
#define DT_NULL      0
#define DT_NEEDED    1
#define DT_HASH      4
#define DT_STRTAB    5
#define DT_SYMTAB    6
#define DT_RELA      7
#define DT_RELASZ    8
#define DT_RELAENT   9
#define DT_STRSZ     10
#define DT_SYMENT    11
#define DT_DEBUG     21
#define DT_FLAGS     30
#define DT_FLAGS_1   0x6ffffffb

static const char interp[] = "/lib64/ld-linux-x86-64.so.2";

/*
 * Startup code (what crt1.o does): the kernel starts the program with
 *     [rsp] = argc, [rsp+8] = argv[0], ...   and rdx = function for exit
 * __libc_start_main(main, argc, argv, init, fini, rtld_fini, stack_end)
 * initializes the C library, calls main and then exit with its result.
 */
static const char* crt1 =
    "    .text\n"
    "_start:\n"
    "    xor ebp, ebp                  # end of the frame chain\n"
    "    mov r9, rdx                   # rtld_fini\n"
    "    pop rsi                       # argc\n"
    "    mov rdx, rsp                  # argv\n"
    "    and rsp, -16\n"
    "    push rax                      # keep rsp 16-aligned\n"
    "    push rsp                      # stack_end\n"
    "    xor r8d, r8d                  # fini\n"
    "    xor ecx, ecx                  # init\n"
    "    lea rdi, [rip+main]\n"
    "    call __libc_start_main\n"
    "    hlt\n";

/*
 * Functions of libc_nonshared.a: glibc keeps them in a static library,
 * so libc.so.6 does not export them. They are small wrappers; the
 * linker adds them when the program uses them.
 */
static const struct
{
    const char* name;
    const char* code;
} nonshared[] = {
    /* atexit(f) is __cxa_atexit(f, NULL, __dso_handle); NULL is fine for the main program */
    { "atexit", "atexit:\n    xor esi, esi\n    xor edx, edx\n    jmp __cxa_atexit\n" },
};

struct import
{
    struct import* next;
    struct symbol* sym;
    int is_data;
    int dynsym;          /* index in .dynsym */
    int name_offset;     /* in .dynstr */
    int got;             /* GOT entry index, -1 if none */
    int thunk;           /* offset of the thunk in .text, functions only */
};

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
        b->capacity = (b->size + n) * 2 + 256;
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

static void section_put(struct section* s, int byte)
{
    if (s->size == s->capacity)
    {
        s->capacity = s->capacity * 2 + 64;
        s->data = realloc(s->data, s->capacity);
        if (s->data == NULL)
        {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
    }
    s->data[s->size++] = (unsigned char)byte;
}

static void w16(unsigned char* p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void w32(unsigned char* p, unsigned v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i)); }
static void w64(unsigned char* p, unsigned long long v) { for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i)); }

static void put64(struct buf* b, unsigned long long v) { unsigned char x[8]; w64(x, v); put(b, x, 8); }

static int align_up(int n, int a)
{
    return (n + a - 1) / a * a;
}

struct map_entry
{
    unsigned long long address;
    const char* name;
};

static int compare_map(const void* a, const void* b)
{
    const struct map_entry* x = a;
    const struct map_entry* y = b;
    return x->address < y->address ? -1 : x->address > y->address;
}

/* "address name" of every symbol, sorted: to find where an address is */
static void write_map(struct object* obj, const char* map_path, unsigned long long text_va, unsigned long long data_va)
{
    int n = 0;
    for (int b = 0; b < SYMBOL_BUCKETS; b++)
        for (struct symbol* s = obj->buckets[b]; s; s = s->next)
            n++;
    struct map_entry* e = cc_alloc(sizeof *e * (n + 1));
    n = 0;
    for (int b = 0; b < SYMBOL_BUCKETS; b++)
        for (struct symbol* s = obj->buckets[b]; s; s = s->next)
            if (s->defined && s->name[0] != '.')
            {
                e[n].address = (s->section == SEC_TEXT ? text_va : data_va) + s->offset;
                e[n].name = s->name;
                n++;
            }
    qsort(e, n, sizeof *e, compare_map);
    FILE* f = fopen(map_path, "w");
    if (f == NULL)
        return;
    for (int i = 0; i < n; i++)
        fprintf(f, "%llx %s\n", e[i].address, e[i].name);
    fclose(f);
}

/* ------------------------------------------------------- debug sections */

static void put8(struct buf* b, int v) { unsigned char x = (unsigned char)v; put(b, &x, 1); }
static void put16(struct buf* b, unsigned v) { unsigned char x[2]; w16(x, v); put(b, x, 2); }
static void put32(struct buf* b, unsigned v) { unsigned char x[4]; w32(x, v); put(b, x, 4); }

static void put_uleb(struct buf* b, unsigned long long v)
{
    do
    {
        int byte = v & 0x7F;
        v >>= 7;
        put8(b, v ? byte | 0x80 : byte);
    } while (v);
}

static void put_sleb(struct buf* b, long long v)
{
    for (;;)
    {
        int byte = v & 0x7F;
        v >>= 7;
        int done = (v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40));
        put8(b, done ? byte : byte | 0x80);
        if (done) break;
    }
}

/*
 * .debug_line (DWARF 4): a header with the list of files, then a small
 * program for a state machine (address, file, line). Each row the
 * assembler collected becomes: set the file, advance the line, advance
 * the address, "copy" (emit a row). gdb runs the program backwards to
 * know which line an address belongs to, and the other way around.
 */
static void build_debug_line(struct object* obj, unsigned long long text_va, struct buf* b)
{
    static const unsigned char opcode_lengths[12] = { 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1 };

    put32(b, 0);                     /* unit length, patched at the end */
    put16(b, 4);                     /* version */
    int header_length_at = b->size;
    put32(b, 0);                     /* header length, patched */
    int header_start = b->size;
    put8(b, 1);                      /* minimum instruction length */
    put8(b, 1);                      /* maximum operations per instruction */
    put8(b, 1);                      /* default is_stmt */
    put8(b, -5);                     /* line base */
    put8(b, 14);                     /* line range */
    put8(b, 13);                     /* opcode base */
    put(b, opcode_lengths, 12);
    put8(b, 0);                      /* no include directories */
    for (int i = 1; i < 256 && obj->files[i]; i++)
    {
        put(b, obj->files[i], (int)strlen(obj->files[i]) + 1);
        put_uleb(b, 0);              /* directory */
        put_uleb(b, 0);              /* time */
        put_uleb(b, 0);              /* size */
    }
    put8(b, 0);
    w32(b->data + header_length_at, (unsigned)(b->size - header_start));

    if (obj->lines)
    {
        /* DW_LNE_set_address */
        unsigned long long address = text_va + obj->lines->offset;
        put8(b, 0);
        put_uleb(b, 9);
        put8(b, 2);
        unsigned char a[8];
        w64(a, address);
        put(b, a, 8);

        int file = 1, line = 1;
        for (struct line_row* r = obj->lines; r; r = r->next)
        {
            unsigned long long row_address = text_va + r->offset;
            if (r->file != file)
            {
                put8(b, 4);                          /* DW_LNS_set_file */
                put_uleb(b, (unsigned)r->file);
                file = r->file;
            }
            if (r->line != line)
            {
                put8(b, 3);                          /* DW_LNS_advance_line */
                put_sleb(b, r->line - line);
                line = r->line;
            }
            if (row_address != address)
            {
                put8(b, 2);                          /* DW_LNS_advance_pc */
                put_uleb(b, row_address - address);
                address = row_address;
            }
            put8(b, 1);                              /* DW_LNS_copy */
        }

        /* the sequence ends after the last function */
        struct func_range* last = obj->funcs;
        while (last && last->next) last = last->next;
        if (last && text_va + last->end > address)
        {
            put8(b, 2);
            put_uleb(b, text_va + last->end - address);
        }
        put8(b, 0);                                  /* DW_LNE_end_sequence */
        put_uleb(b, 1);
        put8(b, 1);
    }
    w32(b->data, (unsigned)(b->size - 4));
}

/* the same .debug_line, for the Mach-O linker (it goes into the .dSYM) */
void dwarf_line_table(struct object* obj, unsigned long long text_va, unsigned char** data, int* size)
{
    struct buf b = { 0 };
    build_debug_line(obj, text_va, &b);
    *data = b.data;
    *size = b.size;
}

/*
 * .debug_frame: how to find the caller's frame at each address (call
 * frame information). Every function starts the same way:
 *     +0  push rbp          CFA = rsp + 8  (CFA: rsp before the call)
 *     +1  mov rbp, rsp      CFA = rsp + 16, rbp saved at CFA - 16
 *     +4  ...               CFA = rbp + 16
 * so one CIE (the common part) and one FDE per function are enough.
 */
static void build_debug_frame(struct object* obj, unsigned long long text_va, struct buf* b)
{
    /* CIE */
    put32(b, 0);                     /* length, patched */
    put32(b, 0xFFFFFFFF);            /* CIE id */
    put8(b, 1);                      /* version */
    put8(b, 0);                      /* augmentation "" */
    put_uleb(b, 1);                  /* code alignment */
    put_sleb(b, -8);                 /* data alignment */
    put8(b, 16);                     /* return address: register 16 (rip) */
    put8(b, 0x0c); put_uleb(b, 7); put_uleb(b, 8);   /* DW_CFA_def_cfa rsp, 8 */
    put8(b, 0x80 | 16); put_uleb(b, 1);              /* DW_CFA_offset rip, CFA-8 */
    while ((b->size) % 8) put8(b, 0);                /* DW_CFA_nop */
    w32(b->data, (unsigned)(b->size - 4));

    for (struct func_range* f = obj->funcs; f; f = f->next)
    {
        int start = b->size;
        put32(b, 0);                 /* length, patched */
        put32(b, 0);                 /* offset of the CIE */
        unsigned char a[8];
        w64(a, text_va + f->start);
        put(b, a, 8);                /* first address */
        w64(a, (unsigned long long)(f->end - f->start));
        put(b, a, 8);                /* size */
        put8(b, 0x40 | 1);                               /* DW_CFA_advance_loc 1 */
        put8(b, 0x0e); put_uleb(b, 16);                  /* DW_CFA_def_cfa_offset 16 */
        put8(b, 0x80 | 6); put_uleb(b, 2);               /* DW_CFA_offset rbp, CFA-16 */
        put8(b, 0x40 | 3);                               /* DW_CFA_advance_loc 3 */
        put8(b, 0x0d); put_uleb(b, 6);                   /* DW_CFA_def_cfa_register rbp */
        while ((b->size - start) % 8) put8(b, 0);
        w32(b->data + start, (unsigned)(b->size - start - 4));
    }
}

/* .symtab: names of the functions and objects, so tools can show them */
static void build_symtab(struct object* obj, unsigned long long text_va, unsigned long long data_va,
                         int text_index, int data_index, struct buf* symtab, struct buf* strtab)
{
    put(symtab, NULL, 24);           /* symbol 0 is empty */
    put8(strtab, 0);
    for (int i = 0; i < SYMBOL_BUCKETS; i++)
    {
        for (struct symbol* s = obj->buckets[i]; s; s = s->next)
        {
            if (!s->defined || s->name[0] == '.' || s->section > SEC_DATA)
                continue;
            unsigned long long size = 0;
            int is_func = 0;
            for (struct func_range* f = obj->funcs; f; f = f->next)
                if (s->section == SEC_TEXT && f->start == s->offset && strcmp(f->name, s->name) == 0)
                {
                    is_func = 1;
                    size = (unsigned long long)(f->end - f->start);
                }
            unsigned char e[24] = { 0 };
            w32(e, (unsigned)strtab->size);
            e[4] = (unsigned char)(1 << 4 | (s->section == SEC_DATA ? 1 : is_func ? 2 : 0));
            w16(e + 6, (unsigned)(s->section == SEC_TEXT ? text_index : data_index));
            w64(e + 8, (s->section == SEC_TEXT ? text_va : data_va) + s->offset);
            w64(e + 16, size);
            put(symtab, e, 24);
            put(strtab, s->name, (int)strlen(s->name) + 1);
        }
    }
}

/* a dynamic section entry */
static void dyn(struct buf* b, long long tag, unsigned long long value)
{
    put64(b, (unsigned long long)tag);
    put64(b, value);
}

void link_elf(struct object* obj, const char** lib_names, int nlibs, const char* path, const char* map_path)
{
    struct section* text = &obj->sections[SEC_TEXT];
    struct section* data = &obj->sections[SEC_DATA];

    struct symbol* main_sym = find_symbol(obj, "main", 0);
    if (main_sym == NULL || !main_sym->defined)
        cc_error(path, 0, 0, "undefined symbol 'main'");
    assemble(crt1, obj);
    for (int i = 0; i < (int)(sizeof nonshared / sizeof nonshared[0]); i++)
    {
        struct symbol* s = find_symbol(obj, nonshared[i].name, 0);
        int used = 0;
        for (struct fixup* f = obj->fixups; f && !used; f = f->next)
            used = strcmp(f->name, nonshared[i].name) == 0;
        if (used && (s == NULL || !s->defined))
        {
            char code[512];
            snprintf(code, sizeof code, "    .text\n%s", nonshared[i].code);
            assemble(code, obj);
        }
    }

    /* 1. imports: names used but not defined, and GOT entries */
    struct import* imports = NULL;
    struct import** tail = &imports;
    int nimports = 0, ngot = 0;
    struct import* got_local[4096];   /* GOT entries of symbols defined here */
    int nlocal = 0;

    for (struct fixup* f = obj->fixups; f; f = f->next)
    {
        struct symbol* s = find_symbol(obj, f->name, 1);
        struct import* im = NULL;
        for (struct import* x = imports; x; x = x->next)
            if (x->sym == s) im = x;
        for (int i = 0; i < nlocal && !im; i++)
            if (got_local[i]->sym == s) im = got_local[i];

        if (im == NULL && (!s->defined || f->kind == FIX_GOTREL32))
        {
            im = cc_alloc(sizeof *im);
            im->sym = s;
            im->is_data = s->is_data;
            im->got = -1;
            if (s->defined)
            {
                if (nlocal == 4096)
                    cc_error(path, 0, 0, "too many GOT entries");
                got_local[nlocal++] = im;
            }
            else
            {
                *tail = im;
                tail = &im->next;
                im->dynsym = ++nimports;
            }
        }
        /* functions always need a GOT entry for their thunk */
        if (im && im->got < 0 && (f->kind == FIX_GOTREL32 || (!s->defined && !im->is_data)))
            im->got = ngot++;
    }

    /* 2. thunks for imported functions, at the end of .text */
    while (text->size % 16)
        section_put(text, 0xCC);
    for (struct import* im = imports; im; im = im->next)
    {
        if (im->is_data)
            continue;
        im->thunk = text->size;
        section_put(text, 0xFF);    /* jmp qword ptr [rip+rel32] */
        section_put(text, 0x25);
        for (int i = 0; i < 4; i++)
            section_put(text, 0);
    }

    /* 3. dynamic string table: names of libraries and symbols */
    struct buf dynstr = { 0 };
    put(&dynstr, "", 1);
    int* lib_name_offsets = cc_alloc(sizeof(int) * (nlibs + 1));
    for (int i = 0; i < nlibs; i++)
    {
        lib_name_offsets[i] = dynstr.size;
        put(&dynstr, lib_names[i], (int)strlen(lib_names[i]) + 1);
    }
    for (struct import* im = imports; im; im = im->next)
    {
        im->name_offset = dynstr.size;
        put(&dynstr, im->sym->name, (int)strlen(im->sym->name) + 1);
    }

    /* relocations, counted now to know the layout */
    int nrela = 0;
    for (struct import* im = imports; im; im = im->next)
        if (im->got >= 0) nrela++;
    for (struct fixup* f = obj->fixups; f; f = f->next)
    {
        struct symbol* s = find_symbol(obj, f->name, 0);
        if (f->kind == FIX_ABS64 && !s->defined && s->is_data)
        {
            if (f->section != SEC_DATA)
                cc_error(path, 0, 0, "address of '%s' in code", f->name);
            nrela++;
        }
    }

    /* 4. layout of segment 1 */
    int off = 64 + NPHDR * 56;
    int interp_off = off;                   off += sizeof interp;
    off = align_up(off, 8);
    int dynsym_off = off;                   off += 24 * (nimports + 1);
    int dynstr_off = off;                   off += dynstr.size;
    off = align_up(off, 8);
    int hash_off = off;                     off += 4 * (2 + 1 + nimports + 1);
    off = align_up(off, 8);
    int rela_off = off;                     off += 24 * nrela;
    off = align_up(off, 16);
    int text_off = off;                     off += text->size;
    int seg1_end = off;

    /* segment 2 starts on a new page */
    int seg2_off = align_up(seg1_end, PAGE);
    off = seg2_off;
    int data_off = off;                     off += data->size;
    off = align_up(off, 8);
    int got_off = off;                      off += 8 * ngot;
    int dyn_off = off;
    int ndyn = nlibs + 12;
    off += 16 * ndyn;
    int seg2_end = off;

    unsigned long long text_va = BASE + text_off;
    unsigned long long data_va = BASE + data_off;
    unsigned long long got_va = BASE + got_off;

    /* addresses of symbols */
    #define SYM_VA(s) ((s)->section == SEC_TEXT ? text_va + (s)->offset : data_va + (s)->offset)

    /* 5. thunks point to their GOT entries */
    for (struct import* im = imports; im; im = im->next)
    {
        if (im->is_data)
            continue;
        unsigned long long next = text_va + im->thunk + 6;
        w32(text->data + im->thunk + 2, (unsigned)(got_va + 8 * im->got - next));
    }

    /* 6. GOT contents and relocations */
    unsigned char* got = cc_alloc(8 * ngot + 8);
    struct buf rela = { 0 };
    for (int i = 0; i < nlocal; i++)
        w64(got + 8 * got_local[i]->got, SYM_VA(got_local[i]->sym));
    for (struct import* im = imports; im; im = im->next)
    {
        if (im->got < 0)
            continue;
        put64(&rela, got_va + 8 * im->got);                                /* r_offset */
        put64(&rela, (unsigned long long)im->dynsym << 32 | R_X86_64_GLOB_DAT); /* r_info */
        put64(&rela, 0);                                                    /* r_addend */
    }

    /* 7. fixups */
    for (struct fixup* f = obj->fixups; f; f = f->next)
    {
        struct symbol* s = find_symbol(obj, f->name, 0);
        struct import* im = NULL;
        for (struct import* x = imports; x; x = x->next)
            if (x->sym == s) im = x;
        for (int i = 0; i < nlocal && !im; i++)
            if (got_local[i]->sym == s) im = got_local[i];

        unsigned long long base = f->section == SEC_TEXT ? text_va : data_va;
        unsigned char* at = obj->sections[f->section].data + f->offset;

        if (f->kind == FIX_GOTREL32)
        {
            w32(at, (unsigned)(got_va + 8 * im->got + f->addend - (base + f->end)));
            continue;
        }

        unsigned long long target;
        if (s->defined)
            target = SYM_VA(s);
        else if (!im->is_data)
            target = text_va + im->thunk;    /* a function of a library: its thunk */
        else
        {
            if (f->kind != FIX_ABS64)
                cc_error(path, 0, 0, "'%s' of a shared library used without its GOT entry", f->name);
            /* address of a library object inside data: the loader writes it */
            put64(&rela, base + f->offset);
            put64(&rela, (unsigned long long)im->dynsym << 32 | R_X86_64_64);
            put64(&rela, (unsigned long long)f->addend);
            w64(at, 0);
            continue;
        }
        target += f->addend;

        if (f->kind == FIX_REL32)
            w32(at, (unsigned)(target - (base + f->end)));
        else
            w64(at, target);
    }

    if (map_path)
        write_map(obj, map_path, text_va, data_va);

    /* 8. the file */
    struct buf out = { 0 };
    put(&out, NULL, seg2_end);
    unsigned char* o = out.data;

    struct symbol* entry = find_symbol(obj, "_start", 0);

    /* ELF header */
    memcpy(o, "\x7f" "ELF", 4);
    o[4] = 2;                                /* 64 bits */
    o[5] = 1;                                /* little endian */
    o[6] = 1;                                /* version */
    w16(o + 16, 2);                          /* executable */
    w16(o + 18, 62);                         /* x86-64 */
    w32(o + 20, 1);
    w64(o + 24, SYM_VA(entry));              /* entry point */
    w64(o + 32, 64);                         /* program headers offset */
    w16(o + 52, 64);                         /* header size */
    w16(o + 54, 56);                         /* program header size */
    w16(o + 56, NPHDR);

    /* program headers: type flags offset vaddr paddr filesz memsz align */
    struct { unsigned type, flags; unsigned long long offset, size, align; } ph[NPHDR] = {
        { PT_PHDR,      4, 64,         NPHDR * 56,             8 },
        { PT_INTERP,    4, interp_off, sizeof interp,          1 },
        { PT_LOAD,      5, 0,          seg1_end,               PAGE },   /* read, execute */
        { PT_LOAD,      6, seg2_off,   seg2_end - seg2_off,    PAGE },   /* read, write */
        { PT_DYNAMIC,   6, dyn_off,    16 * ndyn,              8 },
        { PT_GNU_STACK, 6, 0,          0,                      16 },     /* stack not executable */
    };
    for (int i = 0; i < NPHDR; i++)
    {
        unsigned char* p = o + 64 + 56 * i;
        w32(p, ph[i].type);
        w32(p + 4, ph[i].flags);
        w64(p + 8, ph[i].offset);
        w64(p + 16, ph[i].type == PT_GNU_STACK ? 0 : BASE + ph[i].offset);
        w64(p + 24, ph[i].type == PT_GNU_STACK ? 0 : BASE + ph[i].offset);
        w64(p + 32, ph[i].size);
        w64(p + 40, ph[i].size);
        w64(p + 48, ph[i].align);
    }

    memcpy(o + interp_off, interp, sizeof interp);

    /* dynamic symbols: entry 0 is empty, then every import (undefined) */
    for (struct import* im = imports; im; im = im->next)
    {
        unsigned char* p = o + dynsym_off + 24 * im->dynsym;
        w32(p, (unsigned)im->name_offset);
        p[4] = (unsigned char)(1 << 4 | (im->is_data ? 1 : 2));   /* global, object or function */
    }
    memcpy(o + dynstr_off, dynstr.data, dynstr.size);

    /* hash table with one bucket and empty chains: the program exports nothing */
    w32(o + hash_off, 1);                    /* nbucket */
    w32(o + hash_off + 4, nimports + 1);     /* nchain */

    if (rela.size)
        memcpy(o + rela_off, rela.data, rela.size);
    memcpy(o + text_off, text->data, text->size);
    if (data->size)
        memcpy(o + data_off, data->data, data->size);
    memcpy(o + got_off, got, 8 * ngot);

    struct buf d = { 0 };
    for (int i = 0; i < nlibs; i++)
        dyn(&d, DT_NEEDED, (unsigned long long)lib_name_offsets[i]);
    dyn(&d, DT_HASH, BASE + hash_off);
    dyn(&d, DT_STRTAB, BASE + dynstr_off);
    dyn(&d, DT_SYMTAB, BASE + dynsym_off);
    dyn(&d, DT_STRSZ, (unsigned long long)dynstr.size);
    dyn(&d, DT_SYMENT, 24);
    dyn(&d, DT_RELA, BASE + rela_off);
    dyn(&d, DT_RELASZ, (unsigned long long)rela.size);
    dyn(&d, DT_RELAENT, 24);
    dyn(&d, DT_FLAGS, 8);                    /* DF_BIND_NOW */
    dyn(&d, DT_FLAGS_1, 1);                  /* DF_1_NOW */
    dyn(&d, DT_DEBUG, 0);                    /* used by debuggers */
    dyn(&d, DT_NULL, 0);
    memcpy(o + dyn_off, d.data, d.size);

    /*
     * 9. sections that are not loaded: debug information, the symbol
     * table, and the section headers that describe every part of the
     * file. The loader ignores them; gdb, readelf and objdump use them.
     */
    struct buf debug_line = { 0 }, debug_frame = { 0 }, symtab = { 0 }, strtab = { 0 };
    int has_debug = obj->sections[SEC_DEBUG_INFO].size > 0;
    if (has_debug)
    {
        build_debug_line(obj, text_va, &debug_line);
        build_debug_frame(obj, text_va, &debug_frame);
    }
    build_symtab(obj, text_va, data_va, 7, 8, &symtab, &strtab);   /* 7 = .text, 8 = .data below */

    struct section_out
    {
        const char* name;
        unsigned type;
        unsigned long long flags;
        int offset;             /* in the file; -1 = append the contents */
        const unsigned char* contents;
        int size;
        int link, info, align, entsize;
    } sh[] = {
        { "",              0,  0, 0, NULL, 0, 0, 0, 0, 0 },
        { ".interp",       1,  2, interp_off, NULL, sizeof interp, 0, 0, 1, 0 },
        { ".dynsym",      11,  2, dynsym_off, NULL, 24 * (nimports + 1), 3, 1, 8, 24 },
        { ".dynstr",       3,  2, dynstr_off, NULL, dynstr.size, 0, 0, 1, 0 },
        { ".hash",         5,  2, hash_off, NULL, 4 * (3 + nimports + 1), 2, 0, 8, 4 },
        { ".rela.dyn",     4,  2, rela_off, NULL, rela.size, 2, 0, 8, 24 },
        { ".got",          1,  3, got_off, NULL, 8 * ngot, 0, 0, 8, 8 },
        { ".text",         1,  6, text_off, NULL, text->size, 0, 0, 16, 0 },
        { ".data",         1,  3, data_off, NULL, data->size, 0, 0, 16, 0 },
        { ".dynamic",      6,  3, dyn_off, NULL, 16 * ndyn, 3, 0, 8, 16 },
        { ".symtab",       2,  0, -1, symtab.data, symtab.size, 11, 1, 8, 24 },
        { ".strtab",       3,  0, -1, strtab.data, strtab.size, 0, 0, 1, 0 },
        { ".debug_abbrev", 1,  0, -1, obj->sections[SEC_DEBUG_ABBREV].data, obj->sections[SEC_DEBUG_ABBREV].size, 0, 0, 1, 0 },
        { ".debug_info",   1,  0, -1, obj->sections[SEC_DEBUG_INFO].data, obj->sections[SEC_DEBUG_INFO].size, 0, 0, 1, 0 },
        { ".debug_line",   1,  0, -1, debug_line.data, debug_line.size, 0, 0, 1, 0 },
        { ".debug_frame",  1,  0, -1, debug_frame.data, debug_frame.size, 0, 0, 8, 0 },
        { ".shstrtab",     3,  0, -1, NULL, 0, 0, 0, 1, 0 },
    };
    int nsh = (int)(sizeof sh / sizeof sh[0]);
    if (!has_debug)
    {
        /* drop the four debug sections, keep .shstrtab last */
        sh[nsh - 5] = sh[nsh - 1];
        nsh -= 4;
    }

    struct buf shstrtab = { 0 };
    int name_offsets[32];
    for (int i = 0; i < nsh; i++)
    {
        name_offsets[i] = shstrtab.size;
        put(&shstrtab, sh[i].name, (int)strlen(sh[i].name) + 1);
    }
    sh[nsh - 1].contents = shstrtab.data;
    sh[nsh - 1].size = shstrtab.size;

    for (int i = 1; i < nsh; i++)
    {
        if (sh[i].offset >= 0)
            continue;
        if (sh[i].align > 1)
            while (out.size % sh[i].align) put(&out, "", 1);
        sh[i].offset = out.size;
        if (sh[i].size)
            put(&out, sh[i].contents, sh[i].size);
    }

    while (out.size % 8) put(&out, "", 1);
    int shoff = out.size;
    put(&out, NULL, 64 * nsh);
    for (int i = 1; i < nsh; i++)
    {
        unsigned char* p = out.data + shoff + 64 * i;
        int loaded = sh[i].flags & 2;
        w32(p, (unsigned)name_offsets[i]);
        w32(p + 4, sh[i].type);
        w64(p + 8, sh[i].flags);
        w64(p + 16, loaded ? BASE + sh[i].offset : 0);   /* address in memory */
        w64(p + 24, (unsigned long long)sh[i].offset);
        w64(p + 32, (unsigned long long)sh[i].size);
        w32(p + 40, (unsigned)sh[i].link);
        w32(p + 44, (unsigned)sh[i].info);
        w64(p + 48, (unsigned long long)sh[i].align);
        w64(p + 56, (unsigned long long)sh[i].entsize);
    }
    o = out.data;
    w64(o + 40, (unsigned long long)shoff);   /* section headers offset */
    w16(o + 58, 64);                          /* section header size */
    w16(o + 60, (unsigned)nsh);
    w16(o + 62, (unsigned)(nsh - 1));         /* index of .shstrtab */

    FILE* f = fopen(path, "wb");
    if (f == NULL)
        cc_error(path, 0, 0, "cannot write");
    fwrite(out.data, 1, out.size, f);
    fclose(f);
#ifndef _WIN32
    chmod(path, 0755);
#endif
}
