/*
 * Program database (.pdb) for -g on Windows, read by Visual Studio,
 * WinDbg and cdb.
 *
 * A PDB is a small file system (MSF, "multi-stream file"): the file is
 * divided in blocks of 4096 bytes and holds numbered streams; a
 * directory says which blocks belong to each stream.
 *
 *   block 0      super block: magic, block size, where the directory is
 *   blocks 1, 2  free block maps (one bit per block)
 *   ...          stream data, the directory, the directory's block list
 *
 * Streams written here:
 *    1  PDB info      version, GUID and age (must match the exe), names of named streams
 *    2  TPI           the type records (.debug$T) and
 *    6                their hash table, used to find a struct by name
 *    3  DBI           list of modules, sections, source files, other streams
 *    4  IPI           id records (empty)
 *    5  /names        strings (source file names)
 *    7  module        the symbols of .debug$S, then the line table (C13)
 *    8  symbols       global records: S_PUB32, S_GDATA32, S_PROCREF
 *    9  globals       hash table over the global records
 *   10  publics       hash table over S_PUB32, and a table sorted by address
 *   11  sections      copy of the PE section headers
 *
 * Details follow LLVM's PDB writer and the PDB documentation by
 * Pascal Beyer (github.com/PascalBeyer/PDB-Documentation).
 */
#include "object.h"
#include "cc89.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#define BLOCK 4096

enum stream_index
{
    STREAM_OLD_DIRECTORY, STREAM_INFO, STREAM_TPI, STREAM_DBI, STREAM_IPI,
    STREAM_NAMES, STREAM_TPI_HASH, STREAM_MODULE, STREAM_SYMBOLS,
    STREAM_GLOBALS, STREAM_PUBLICS, STREAM_SECTIONS, STREAM_COUNT
};

/* symbol kinds used here */
#define S_LDATA32    0x110c
#define S_GDATA32    0x110d
#define S_PUB32      0x110e
#define S_LPROC32    0x110f
#define S_GPROC32    0x1110
#define S_PROCREF    0x1125
#define S_LPROCREF   0x1127

struct buf
{
    unsigned char* data;
    int size, capacity;
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

static void u8(struct buf* b, int v) { unsigned char x = (unsigned char)v; put(b, &x, 1); }
static void u16(struct buf* b, unsigned v) { u8(b, (int)v); u8(b, (int)(v >> 8)); }
static void u32(struct buf* b, unsigned v) { for (int i = 0; i < 4; i++) u8(b, (int)(v >> (8 * i))); }
static void str(struct buf* b, const char* s) { put(b, s, (int)strlen(s) + 1); }
static void align4(struct buf* b) { while (b->size % 4) u8(b, 0); }

static unsigned rd16(const unsigned char* p) { return p[0] | p[1] << 8; }
static unsigned rd32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static void wr32(unsigned char* p, unsigned v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i)); }

/* ---------------------------------------------------------------- hashes */

/* the string hash of PDB hash tables (LLVM: hashStringV1) */
static unsigned hash_string(const char* s)
{
    unsigned n = (unsigned)strlen(s), result = 0, i = 0;
    for (; i + 4 <= n; i += 4)
        result ^= rd32((const unsigned char*)s + i);
    if (n - i >= 2)
    {
        result ^= rd16((const unsigned char*)s + i);
        i += 2;
    }
    if (n - i == 1)
        result ^= (unsigned char)s[i];
    result |= 0x20202020;              /* ignore the case of letters */
    result ^= result >> 11;
    return result ^ (result >> 16);
}

/* CRC-32 starting at 0 and without the final inversion (LLVM: hashBufferV8) */
static unsigned crc32_jam(const unsigned char* p, int n)
{
    unsigned crc = 0;
    for (int i = 0; i < n; i++)
    {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return crc;
}

/* ------------------------------------------------------------- /names */

struct names
{
    struct buf strings;          /* starts with an empty string at offset 0 */
    int offsets[256];
    int count;
};

static int names_add(struct names* nm, const char* s)
{
    for (int i = 0; i < nm->count; i++)
        if (strcmp((const char*)nm->strings.data + nm->offsets[i], s) == 0)
            return nm->offsets[i];
    if (nm->strings.size == 0)
        u8(&nm->strings, 0);
    int offset = nm->strings.size;
    str(&nm->strings, s);
    if (nm->count < 256)
        nm->offsets[nm->count++] = offset;
    return offset;
}

/* string table: signature, version, strings, hash table (linear probing), count */
static void write_string_table(struct buf* b, struct names* nm)
{
    if (nm->strings.size == 0)
        u8(&nm->strings, 0);
    u32(b, 0xEFFEEFFE);
    u32(b, 1);
    u32(b, (unsigned)nm->strings.size);
    put(b, nm->strings.data, nm->strings.size);
    unsigned nbuckets = (unsigned)nm->count * 2 + 1;
    unsigned* buckets = cc_alloc(sizeof(unsigned) * nbuckets);
    for (int i = 0; i < nm->count; i++)
    {
        unsigned h = hash_string((const char*)nm->strings.data + nm->offsets[i]) % nbuckets;
        while (buckets[h]) h = (h + 1) % nbuckets;
        buckets[h] = (unsigned)nm->offsets[i];
    }
    u32(b, nbuckets);
    for (unsigned i = 0; i < nbuckets; i++)
        u32(b, buckets[i]);
    u32(b, (unsigned)nm->count);
    free(buckets);
}

/* ------------------------------------------------------- GSI hash table */

struct gsi_entry
{
    const char* name;
    unsigned offset;             /* of the record in the symbol record stream */
    int is_static;
    unsigned bucket;
    int section;                 /* publics: for the address map */
    unsigned address;
};

static int lower(int c) { return tolower((unsigned char)c); }

/* order inside a bucket: exported first, then shorter names, then by name ignoring case */
static int compare_gsi(const void* a, const void* b)
{
    const struct gsi_entry* x = a;
    const struct gsi_entry* y = b;
    if (x->bucket != y->bucket) return x->bucket < y->bucket ? -1 : 1;
    if (x->is_static != y->is_static) return x->is_static - y->is_static;
    size_t lx = strlen(x->name), ly = strlen(y->name);
    if (lx != ly) return lx < ly ? -1 : 1;
    for (size_t i = 0; i < lx; i++)
        if (lower(x->name[i]) != lower(y->name[i]))
            return lower(x->name[i]) - lower(y->name[i]);
    return 0;
}

#define GSI_BUCKETS 4096

/*
 *   header: -1, version, size of the records, size of the bucket information
 *   records: { offset + 1, reference count }   sorted by bucket
 *   bitmap: one bit per bucket that has records (4096 + 32 bits)
 *   for each such bucket: index of its first record * 12
 */
static void write_gsi(struct buf* b, struct gsi_entry* e, int n)
{
    for (int i = 0; i < n; i++)
        e[i].bucket = hash_string(e[i].name) % GSI_BUCKETS;
    qsort(e, n, sizeof *e, compare_gsi);

    unsigned bitmap[(GSI_BUCKETS + 32) / 32] = { 0 };
    int nonempty = 0;
    for (int i = 0; i < n; i++)
        if (i == 0 || e[i].bucket != e[i - 1].bucket)
        {
            bitmap[e[i].bucket / 32] |= 1u << (e[i].bucket % 32);
            nonempty++;
        }

    u32(b, 0xFFFFFFFF);
    u32(b, 0xEFFE0000 + 19990810);
    u32(b, (unsigned)(8 * n));
    u32(b, (unsigned)(sizeof bitmap + 4 * nonempty));
    for (int i = 0; i < n; i++)
    {
        u32(b, e[i].offset + 1);
        u32(b, 1);
    }
    for (int i = 0; i < (int)(sizeof bitmap / 4); i++)
        u32(b, bitmap[i]);
    for (int i = 0; i < n; i++)
        if (i == 0 || e[i].bucket != e[i - 1].bucket)
            u32(b, (unsigned)(i * 12));
}

static int compare_address(const void* a, const void* b)
{
    const struct gsi_entry* x = *(const struct gsi_entry* const*)a;
    const struct gsi_entry* y = *(const struct gsi_entry* const*)b;
    if (x->section != y->section) return x->section - y->section;
    if (x->address != y->address) return x->address < y->address ? -1 : 1;
    return strcmp(x->name, y->name);
}

/* --------------------------------------------------------------- types */

/* name of a struct/union record, or NULL; *forward set for forward references */
static const char* record_name(const unsigned char* r, int* forward)
{
    unsigned kind = rd16(r + 2);
    if (kind != 0x1505 && kind != 0x1506)
        return NULL;
    *forward = (rd16(r + 6) & 0x80) != 0;
    const unsigned char* p = r + 4 + (kind == 0x1505 ? 16 : 8);
    unsigned v = rd16(p);
    p += v < 0x8000 ? 2 : v == 0x8004 ? 6 : 10;     /* the size: numeric leaf */
    return (const char*)p;
}

/* hash of a type record, for the TPI hash stream */
static unsigned type_hash(const unsigned char* r, int size)
{
    int forward = 0;
    const char* name = record_name(r, &forward);
    if (name && !forward)
        return hash_string(name);
    return crc32_jam(r, size);
}

/* ------------------------------------------------------------ module */

/* find the function that contains offset, NULL if none */
static struct func_range* function_at(struct object* obj, int offset)
{
    for (struct func_range* f = obj->funcs; f; f = f->next)
        if (offset >= f->start && offset < f->end)
            return f;
    return NULL;
}

/*
 * C13 line information: a DEBUG_S_FILECHKSMS subsection (one entry per
 * file) and one DEBUG_S_LINES subsection per function:
 *     offset, section, flags, size of the function
 *     blocks: file, number of lines, size, then { offset, line } pairs
 */
static void write_lines(struct buf* b, struct object* obj, struct names* nm)
{
    int nfiles = 0;
    while (nfiles + 1 < 256 && obj->files[nfiles + 1]) nfiles++;

    u32(b, 0xF4);                         /* DEBUG_S_FILECHKSMS */
    u32(b, (unsigned)(8 * nfiles));
    for (int i = 1; i <= nfiles; i++)
    {
        u32(b, (unsigned)names_add(nm, obj->files[i]));
        u8(b, 0);                         /* no checksum */
        u8(b, 0);
        u16(b, 0);                        /* padding */
    }

    for (struct func_range* f = obj->funcs; f; f = f->next)
    {
        struct line_row* first = NULL;
        int nrows = 0;
        for (struct line_row* r = obj->lines; r; r = r->next)
            if (r->offset >= f->start && r->offset < f->end)
            {
                if (!first) first = r;
                nrows++;
            }
        if (nrows == 0)
            continue;

        struct buf s = { 0 };
        u32(&s, (unsigned)f->start);      /* offset in the section */
        u16(&s, 1);                       /* section: .text */
        u16(&s, 0);                       /* flags: no columns */
        u32(&s, (unsigned)(f->end - f->start));

        /* one block for each run of rows from the same file */
        struct line_row* r = first;
        while (r && r->offset < f->end)
        {
            int file = r->file, count = 0;
            struct line_row* q = r;
            while (q && q->offset < f->end && q->file == file) { count++; q = q->next; }
            u32(&s, (unsigned)(8 * (file - 1)));   /* entry in FILECHKSMS */
            u32(&s, (unsigned)count);
            u32(&s, (unsigned)(12 + 8 * count));
            for (; r != q; r = r->next)
            {
                u32(&s, (unsigned)(r->offset - f->start));
                u32(&s, (unsigned)r->line | 0x80000000u);   /* is a statement */
            }
        }
        u32(b, 0xF2);                     /* DEBUG_S_LINES */
        u32(b, (unsigned)s.size);
        put(b, s.data, s.size);
        align4(b);
        free(s.data);
    }
}

/* ------------------------------------------------------------- MSF file */

static int is_fpm_block(int block)
{
    return block % BLOCK == 1 || block % BLOCK == 2;
}

static void write_msf(const char* path, struct buf* streams, int nstreams)
{
    int next = 3;                         /* 0 super block, 1 and 2 free block maps */
    int* first_block = cc_alloc(sizeof(int) * nstreams);
    struct buf directory = { 0 };
    u32(&directory, (unsigned)nstreams);
    for (int i = 0; i < nstreams; i++)
        u32(&directory, (unsigned)streams[i].size);

    /* blocks of each stream: the next free ones */
    struct buf blocks = { 0 };            /* block numbers, stream after stream */
    for (int i = 0; i < nstreams; i++)
    {
        first_block[i] = blocks.size / 4;
        int n = (streams[i].size + BLOCK - 1) / BLOCK;
        for (int k = 0; k < n; k++)
        {
            while (is_fpm_block(next)) next++;
            u32(&blocks, (unsigned)next++);
            u32(&directory, (unsigned)next - 1);
        }
    }

    /* the directory, then one block with the list of its blocks */
    int ndir = (directory.size + BLOCK - 1) / BLOCK;
    int* dir_blocks = cc_alloc(sizeof(int) * (ndir + 1));
    for (int k = 0; k < ndir; k++)
    {
        while (is_fpm_block(next)) next++;
        dir_blocks[k] = next++;
    }
    while (is_fpm_block(next)) next++;
    int block_map = next++;
    int nblocks = next;

    unsigned char* file = cc_alloc((size_t)nblocks * BLOCK);

    /* super block */
    static const char magic[32] = "Microsoft C/C++ MSF 7.00\r\n\x1a" "DS\0\0\0";
    memcpy(file, magic, 32);
    wr32(file + 32, BLOCK);
    wr32(file + 36, 1);                   /* active free block map */
    wr32(file + 40, (unsigned)nblocks);
    wr32(file + 44, (unsigned)directory.size);
    wr32(file + 52, (unsigned)block_map);

    /* free block maps: a bit per block, 1 = free; every block is used */
    memset(file + BLOCK, 0xFF, BLOCK);
    memset(file + 2 * BLOCK, 0xFF, BLOCK);
    for (int k = 0; k < nblocks && k < BLOCK * 8; k++)
        file[BLOCK + k / 8] &= (unsigned char)~(1 << (k % 8));

    for (int i = 0; i < nstreams; i++)
    {
        int n = (streams[i].size + BLOCK - 1) / BLOCK;
        for (int k = 0; k < n; k++)
        {
            int block = (int)rd32(blocks.data + 4 * (first_block[i] + k));
            int size = streams[i].size - k * BLOCK;
            memcpy(file + (size_t)block * BLOCK, streams[i].data + k * BLOCK, size < BLOCK ? size : BLOCK);
        }
    }
    for (int k = 0; k < ndir; k++)
    {
        int size = directory.size - k * BLOCK;
        memcpy(file + (size_t)dir_blocks[k] * BLOCK, directory.data + k * BLOCK, size < BLOCK ? size : BLOCK);
        wr32(file + (size_t)block_map * BLOCK + 4 * k, (unsigned)dir_blocks[k]);
    }

    FILE* f = fopen(path, "wb");
    if (f == NULL)
        cc_error(path, 0, 0, "cannot write");
    fwrite(file, 1, (size_t)nblocks * BLOCK, f);
    fclose(f);
    free(file);
}

/* ---------------------------------------------------------------- PDB */

void write_pdb(struct object* obj, const struct pdb_info* info)
{
    struct buf s[STREAM_COUNT] = { { 0 } };
    struct names names = { 0 };
    struct section* types = &obj->sections[SEC_DEBUG_T];
    struct section* syms = &obj->sections[SEC_DEBUG_S];

    /* 1. sizes of the functions, which only the linker knows */
    for (int at = 0; at < syms->size;)
    {
        unsigned char* r = syms->data + at;
        unsigned kind = rd16(r + 2);
        if (kind == S_GPROC32 || kind == S_LPROC32)
        {
            struct func_range* f = function_at(obj, (int)rd32(r + 32));
            if (f)
            {
                int size = f->end - f->start;
                wr32(r + 16, (unsigned)size);         /* code size */
                wr32(r + 20, 4);                      /* debug start: after push rbp; mov rbp, rsp */
                wr32(r + 24, (unsigned)(size - 1));   /* debug end: the ret */
            }
        }
        at += 2 + (int)rd16(r);
    }

    /* 2. module stream: signature, symbols, line table, global references */
    struct buf* m = &s[STREAM_MODULE];
    u32(m, 4);                            /* CV_SIGNATURE_C13 */
    put(m, syms->data, syms->size);
    int symbols_size = m->size;
    write_lines(m, obj, &names);
    int c13_size = m->size - symbols_size;
    u32(m, 0);

    /* 3. symbol record stream and the lists for the two hash tables */
    struct buf* sr = &s[STREAM_SYMBOLS];
    int max_entries = 64;
    for (int i = 0; i < SYMBOL_BUCKETS; i++)
        for (struct symbol* x = obj->buckets[i]; x; x = x->next)
            max_entries++;
    for (int at = 0; at < syms->size; at += 2 + (int)rd16(syms->data + at))
        max_entries++;
    struct gsi_entry* globals = cc_alloc(sizeof(struct gsi_entry) * max_entries);
    struct gsi_entry* publics = cc_alloc(sizeof(struct gsi_entry) * max_entries);
    int nglobals = 0, npublics = 0;

    for (int at = 0; at < syms->size;)
    {
        unsigned char* r = syms->data + at;
        int length = 2 + (int)rd16(r);
        unsigned kind = rd16(r + 2);
        if (kind == S_GDATA32 || kind == S_LDATA32)
        {
            /* the same record, also in the global stream */
            globals[nglobals].name = (const char*)r + 14;
            globals[nglobals].offset = (unsigned)sr->size;
            globals[nglobals].is_static = kind == S_LDATA32;
            nglobals++;
            put(sr, r, length);
        }
        else if (kind == S_GPROC32 || kind == S_LPROC32)
        {
            /* S_PROCREF: where the function is in the module stream */
            const char* name = (const char*)r + 39;
            globals[nglobals].name = name;
            globals[nglobals].offset = (unsigned)sr->size;
            globals[nglobals].is_static = kind == S_LPROC32;
            nglobals++;
            int start = sr->size;
            u16(sr, 0);
            u16(sr, kind == S_GPROC32 ? S_PROCREF : S_LPROCREF);
            u32(sr, 0);
            u32(sr, (unsigned)(4 + at));  /* offset in the module stream */
            u16(sr, 1);                   /* module 1 (counting from 1) */
            str(sr, name);
            align4(sr);
            sr->data[start] = (unsigned char)(sr->size - start - 2);
            sr->data[start + 1] = (unsigned char)((sr->size - start - 2) >> 8);
        }
        at += length;
    }

    /* S_PUB32 for every named symbol of the program */
    for (int i = 0; i < SYMBOL_BUCKETS; i++)
    {
        for (struct symbol* x = obj->buckets[i]; x; x = x->next)
        {
            if (!x->defined || x->name[0] == '.' || x->section > SEC_DATA || x->offset < 0)
                continue;
            publics[npublics].name = x->name;
            publics[npublics].offset = (unsigned)sr->size;
            publics[npublics].section = x->section + 1;
            publics[npublics].address = (unsigned)x->offset;
            npublics++;
            int start = sr->size;
            u16(sr, 0);
            u16(sr, S_PUB32);
            u32(sr, x->section == SEC_TEXT ? 2 : 0);   /* flags: function */
            u32(sr, (unsigned)x->offset);
            u16(sr, (unsigned)(x->section + 1));
            str(sr, x->name);
            align4(sr);
            sr->data[start] = (unsigned char)(sr->size - start - 2);
            sr->data[start + 1] = (unsigned char)((sr->size - start - 2) >> 8);
        }
    }

    /* 4. globals and publics hash streams */
    write_gsi(&s[STREAM_GLOBALS], globals, nglobals);

    struct buf hash = { 0 };
    write_gsi(&hash, publics, npublics);
    struct gsi_entry** by_address = cc_alloc(sizeof(struct gsi_entry*) * (npublics + 1));
    for (int i = 0; i < npublics; i++)
        by_address[i] = &publics[i];
    qsort(by_address, npublics, sizeof *by_address, compare_address);
    struct buf* p = &s[STREAM_PUBLICS];
    u32(p, (unsigned)hash.size);           /* size of the hash table */
    u32(p, (unsigned)(4 * npublics));      /* size of the address map */
    u32(p, 0);                             /* thunks */
    u32(p, 0);
    u16(p, 0);
    u16(p, 0);
    u32(p, 0);
    u32(p, 0);                             /* sections */
    put(p, hash.data, hash.size);
    for (int i = 0; i < npublics; i++)
        u32(p, by_address[i]->offset);

    /* 5. types: TPI with its hash stream, and an empty IPI */
    int ntypes = 0;
    for (int at = 0; at < types->size; at += 2 + (int)rd16(types->data + at))
        ntypes++;
    struct buf* th = &s[STREAM_TPI_HASH];
    for (int at = 0; at < types->size; at += 2 + (int)rd16(types->data + at))
        u32(th, type_hash(types->data + at, 2 + (int)rd16(types->data + at)) % 0x3FFFF);
    int hash_values_size = th->size;
    u32(th, 0x1000);                       /* index offsets: first type at offset 0 */
    u32(th, 0);

    for (int which = 0; which < 2; which++)
    {
        struct buf* t = &s[which == 0 ? STREAM_TPI : STREAM_IPI];
        int n = which == 0 ? ntypes : 0;
        u32(t, 20040203);                  /* version */
        u32(t, 56);                        /* header size */
        u32(t, 0x1000);                    /* first type index */
        u32(t, (unsigned)(0x1000 + n));
        u32(t, (unsigned)(which == 0 ? types->size : 0));
        u16(t, which == 0 ? STREAM_TPI_HASH : 0xFFFF);
        u16(t, 0xFFFF);
        u32(t, 4);                         /* hash key size */
        u32(t, 0x3FFFF);                   /* hash buckets */
        u32(t, 0);
        u32(t, (unsigned)(which == 0 ? hash_values_size : 0));
        u32(t, (unsigned)(which == 0 ? hash_values_size : 0));
        u32(t, which == 0 ? 8 : 0);
        u32(t, (unsigned)(which == 0 ? hash_values_size + 8 : 0));
        u32(t, 0);
        if (which == 0)
            put(t, types->data, types->size);
    }

    /* 6. DBI */
    int nfiles = 0;
    while (nfiles + 1 < 256 && obj->files[nfiles + 1]) nfiles++;
    const char* module_name = nfiles ? obj->files[1] : "cc89";
    struct func_range* last = obj->funcs;
    while (last && last->next) last = last->next;
    int code_size = last ? last->end : 0;

    struct buf modinfo = { 0 };
    u32(&modinfo, 0);
    u16(&modinfo, 1); u16(&modinfo, 0);   /* first contribution: section 1 */
    u32(&modinfo, 0);
    u32(&modinfo, (unsigned)code_size);
    u32(&modinfo, 0x60000020);
    u16(&modinfo, 0); u16(&modinfo, 0);
    u32(&modinfo, 0); u32(&modinfo, 0);
    u16(&modinfo, 0);                     /* flags */
    u16(&modinfo, STREAM_MODULE);
    u32(&modinfo, (unsigned)symbols_size);
    u32(&modinfo, 0);                     /* old line info */
    u32(&modinfo, (unsigned)c13_size);
    u16(&modinfo, (unsigned)nfiles);
    u16(&modinfo, 0);
    u32(&modinfo, 0);
    u32(&modinfo, 0);
    u32(&modinfo, 0);
    str(&modinfo, module_name);
    str(&modinfo, module_name);
    align4(&modinfo);

    struct buf contrib = { 0 };
    u32(&contrib, 0xEFFE0000 + 19970605);
    u16(&contrib, 1); u16(&contrib, 0);
    u32(&contrib, 0);
    u32(&contrib, (unsigned)code_size);
    u32(&contrib, 0x60000020);
    u16(&contrib, 0); u16(&contrib, 0);
    u32(&contrib, 0); u32(&contrib, 0);

    struct buf secmap = { 0 };
    u16(&secmap, (unsigned)(info->nsections + 1));
    u16(&secmap, (unsigned)(info->nsections + 1));
    for (int i = 0; i < info->nsections; i++)
    {
        const unsigned char* h = info->section_headers + 40 * i;
        unsigned c = rd32(h + 36);
        unsigned flags = 0x108;                       /* selector, 32-bit address */
        if (c & 0x40000000) flags |= 1;               /* read */
        if (c & 0x80000000) flags |= 2;               /* write */
        if (c & 0x20000000) flags |= 4;               /* execute */
        u16(&secmap, flags);
        u16(&secmap, 0); u16(&secmap, 0);
        u16(&secmap, (unsigned)(i + 1));              /* frame */
        u16(&secmap, 0xFFFF); u16(&secmap, 0xFFFF);
        u32(&secmap, 0);
        u32(&secmap, rd32(h + 8));                    /* size */
    }
    u16(&secmap, 0x208);                              /* absolute address */
    u16(&secmap, 0); u16(&secmap, 0);
    u16(&secmap, (unsigned)(info->nsections + 1));
    u16(&secmap, 0xFFFF); u16(&secmap, 0xFFFF);
    u32(&secmap, 0);
    u32(&secmap, 0xFFFFFFFF);

    struct buf fileinfo = { 0 };
    u16(&fileinfo, 1);                    /* modules */
    u16(&fileinfo, (unsigned)nfiles);
    u16(&fileinfo, 0);                    /* first file of module 0 */
    u16(&fileinfo, (unsigned)nfiles);     /* files of module 0 */
    struct buf file_names = { 0 };
    for (int i = 1; i <= nfiles; i++)
    {
        u32(&fileinfo, (unsigned)file_names.size);
        str(&file_names, obj->files[i]);
    }
    put(&fileinfo, file_names.data, file_names.size);
    align4(&fileinfo);

    struct names ec = { 0 };
    struct buf ec_table = { 0 };
    write_string_table(&ec_table, &ec);

    struct buf dbg = { 0 };
    for (int i = 0; i < 11; i++)
        u16(&dbg, i == 5 ? STREAM_SECTIONS : 0xFFFF);   /* only the section headers */

    struct buf* d = &s[STREAM_DBI];
    u32(d, 0xFFFFFFFF);
    u32(d, 19990903);
    u32(d, info->age);
    u16(d, STREAM_GLOBALS);
    u16(d, 0x8E0B);                       /* toolchain 14.11, new format */
    u16(d, STREAM_PUBLICS);
    u16(d, 0);
    u16(d, STREAM_SYMBOLS);
    u16(d, 0);
    u32(d, (unsigned)modinfo.size);
    u32(d, (unsigned)contrib.size);
    u32(d, (unsigned)secmap.size);
    u32(d, (unsigned)fileinfo.size);
    u32(d, 0);                            /* type server map */
    u32(d, 0);
    u32(d, (unsigned)dbg.size);
    u32(d, (unsigned)ec_table.size);
    u16(d, 0);                            /* flags */
    u16(d, 0x8664);                       /* machine */
    u32(d, 0);
    put(d, modinfo.data, modinfo.size);
    put(d, contrib.data, contrib.size);
    put(d, secmap.data, secmap.size);
    put(d, fileinfo.data, fileinfo.size);
    put(d, ec_table.data, ec_table.size);
    put(d, dbg.data, dbg.size);

    put(&s[STREAM_SECTIONS], info->section_headers, 40 * info->nsections);

    /* 7. /names (after the line table added the file names) */
    write_string_table(&s[STREAM_NAMES], &names);

    /* 8. PDB info: version, time, age, GUID, named streams, features */
    struct buf* in = &s[STREAM_INFO];
    u32(in, 20000404);
    u32(in, (unsigned)time(NULL));
    u32(in, info->age);
    put(in, info->guid, 16);
    u32(in, 7);                           /* string buffer */
    str(in, "/names");
    u32(in, 1);                           /* hash table: 1 entry */
    u32(in, 1);                           /* capacity */
    u32(in, 1);                           /* present bits: 1 word */
    u32(in, 1);
    u32(in, 0);                           /* deleted bits: 0 words */
    u32(in, 0);                           /* key: offset of "/names" */
    u32(in, STREAM_NAMES);                /* value: stream */
    u32(in, 0);
    u32(in, 20140508);                    /* feature: VC140 (the IPI stream is valid) */

    write_msf(info->path, s, STREAM_COUNT);
}
