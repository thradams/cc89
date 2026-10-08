#ifndef OBJECT_H
#define OBJECT_H

/*
 * Object produced by the assembler and consumed by the linker:
 * section bytes, symbols and the places that still need an address.
 */

enum section_id
{
    SEC_TEXT,
    SEC_DATA,
    SEC_DEBUG_INFO,     /* -g: DWARF written by the back-end */
    SEC_DEBUG_ABBREV,
    SEC_DEBUG_S,        /* -g on Windows: CodeView symbols */
    SEC_DEBUG_T,        /* -g on Windows: CodeView types */
    SEC_COUNT
};

struct section
{
    unsigned char* data;
    int size;
    int capacity;
};

struct symbol
{
    struct symbol* next;    /* hash chain */
    const char* name;
    int section;
    int offset;
    int defined;
    int selectany;          /* may be defined again: the first definition wins */
    int is_data;            /* .extern_data: an object of a shared library, not a function */
};

enum fixup_kind
{
    FIX_REL32,              /* S + A - P: distance from the end of the instruction */
    FIX_ABS64,              /* image base + S + A: absolute address */
    FIX_GOTREL32,           /* G + A - P: distance to the GOT entry of the symbol */
    FIX_SECREL32,           /* S + A - start of its section: offset inside the section */
    FIX_SECIDX,             /* 2 bytes: number of the section of S (1 = .text) */

    /* AArch64: the value goes inside the bits of an instruction */
    FIX_A64_CALL26,         /* b, bl:       (S - P) / 4 in 26 bits */
    FIX_A64_COND19,         /* b.cond, cbz: (S - P) / 4 in 19 bits */
    FIX_A64_PAGE21,         /* adrp:        page(S) - page(P), pages of 4096 */
    FIX_A64_LO12,           /* add :lo12:   S & 4095 */
    FIX_A64_GOT_PAGE21,     /* adrp :got:   page of the GOT entry of S */
    FIX_A64_GOT_LO12        /* ldr :got_lo12:  (GOT entry & 4095) / 8 */
};

struct fixup
{
    struct fixup* next;
    enum fixup_kind kind;
    int section;
    int offset;             /* where the 4 or 8 bytes are */
    int end;                /* FIX_REL32: offset of the next instruction */
    const char* name;
    long long addend;
};

/* one row of the line table: the code at offset comes from file:line */
struct line_row
{
    struct line_row* next;
    int offset;             /* in .text */
    int file;
    int line;
};

/* code of one function, for symbols and call frame information */
struct func_range
{
    struct func_range* next;
    const char* name;
    int start;              /* offsets in .text */
    int end;
};

#define SYMBOL_BUCKETS 4096

struct object
{
    int arm64;                  /* instruction set: 0 = x86-64, 1 = AArch64 */
    struct section sections[SEC_COUNT];
    struct symbol* buckets[SYMBOL_BUCKETS];
    struct fixup* fixups;

    /* debug information (-g), collected by the assembler */
    const char* files[256];     /* .file N "name" */
    struct line_row* lines;     /* .loc: address -> line, in order */
    struct line_row* lines_tail;
    struct func_range* funcs;   /* .func / .endfunc */
    struct func_range* funcs_tail;
};

struct symbol* find_symbol(struct object* obj, const char* name, int create);

/* assembler: text (written by backend_win64) -> object */
void assemble(const char* text, struct object* obj);

/* linker: object + DLLs (plus a C runtime it chooses) -> PE executable */
void link_exe(struct object* obj, const char** dlls, int ndlls, const char* path);

/* linker part of -g on Windows: writes the .pdb (pdb.c) */
struct pdb_info
{
    const char* path;               /* the .pdb */
    unsigned char guid[16];         /* also in the executable (RSDS record) */
    unsigned age;
    const unsigned char* section_headers;   /* PE section headers, 40 bytes each */
    int nsections;
};
void write_pdb(struct object* obj, const struct pdb_info* info);

/* AArch64 fixups: write target address S into the instruction at P */
void a64_patch(unsigned char* at, enum fixup_kind kind, unsigned long long S, unsigned long long P);

/* linker: object + shared libraries -> ELF executable (Linux x64) */
void link_elf(struct object* obj, const char** libs, int nlibs, const char* path, const char* map_path);

/* -g: the .debug_line contents for code that starts at text_va (elf.c) */
void dwarf_line_table(struct object* obj, unsigned long long text_va, unsigned char** data, int* size);

/* linker: object + dylibs -> Mach-O executable (macOS arm64), with an ad-hoc signature */
void link_macho(struct object* obj, const char** libs, int nlibs, const char* path, const char* map_path);

#endif
