/*
 * cc89 - minimal C89 compiler.
 *
 * The input is C that is already valid (normally the output of cake),
 * so the front-end only reports errors that make code generation
 * impossible. See SPEC.md.
 *
 * Pipeline:
 *     text -> lexer -> tokens -> parser (+ types) -> typed AST -> back-end
 *
 * This header is the contract between the front-end and the back-ends.
 * A back-end only reads the structures below.
 */
#ifndef CC89_H
#define CC89_H

#include <stdio.h>

/* ---------------------------------------------------------------- target */

enum target_kind
{
    TARGET_WIN64,          /* Windows x64: LLP64, Microsoft ABI, PE */
    TARGET_LINUX_X64,      /* Linux x64: LP64, System V ABI, ELF */
    TARGET_MACOS_ARM64,    /* macOS arm64: LP64, Apple AAPCS64, Mach-O */
    TARGET_LINUX_ARM64     /* Linux arm64: LP64, AAPCS64, ELF */
};

enum va_list_kind
{
    VA_CHAR_PTR,            /* char*: Windows, macOS arm64 */
    VA_SYSV,                /* System V x64: array of one struct */
    VA_AAPCS                /* AAPCS64 (Linux arm64): a struct */
};

/* what the front-end needs to know about the target (spec 3.3) */
struct target
{
    enum target_kind kind;
    const char* name;
    int long_size;          /* 4 (LLP64) or 8 (LP64) */
    int long_double_size;   /* 8 = same as double, 16 = x87 or 128-bit (not supported in code) */
    int wchar_size;         /* 2 (UTF-16, unsigned) or 4 (UTF-32, int) */
    int gcc_bitfields;      /* bit-field layout of gcc/clang, else MSVC */
    enum va_list_kind va_list_kind;
    int char_unsigned;      /* plain char is unsigned (Linux arm64) */
    int is_arm64;
};

const struct target* find_target(const char* name);

/* ---------------------------------------------------------------- tokens */

enum token_kind
{
    TK_EOF,
    TK_IDENT,
    TK_INT,      /* integer and character constants */
    TK_FLOAT,
    TK_STRING,
    TK_PRAGMA,   /* whole #pragma line */
    TK_PUNCT,    /* operators and punctuators, text in token.text */
    TK_KEYWORD
};

struct token
{
    enum token_kind kind;
    const char* text;      /* zero terminated copy of the lexeme */
    const char* file;
    int line;
    int col;

    /* TK_INT */
    unsigned long long ival;
    int is_unsigned;       /* type chosen from value and suffix */
    int is_long_long;      /* the type is long long, else int/long */

    /* TK_FLOAT */
    double fval;
    int is_float;          /* suffix f */

    /* TK_STRING: bytes after escapes */
    char* str;
    int str_len;           /* without the final zero */
    int is_wide;           /* L"...": the bytes are UTF-8, wchar_t is UTF-16 */
};

struct token_list
{
    struct token* data;
    int size;
    int capacity;
};

void lex_file(const char* path, struct token_list* out, const struct target* target);

/* ----------------------------------------------------------------- types */

enum type_kind
{
    TY_VOID,
    TY_CHAR,
    TY_SHORT,
    TY_INT,
    TY_LONG,
    TY_LLONG,
    TY_FLOAT,
    TY_DOUBLE,
    TY_LDOUBLE,
    TY_PTR,
    TY_ARRAY,
    TY_FUNC,
    TY_STRUCT,
    TY_UNION
};

struct member
{
    struct member* next;
    const char* name;      /* NULL for unnamed bit-fields */
    struct type* type;
    int offset;
    int bit_width;         /* 0 when it is not a bit-field */
    int bit_offset;
    int is_bitfield;
};

struct param
{
    struct param* next;
    struct type* type;
    const struct token* tok;     /* name of the parameter, for debug info */
    const char* name;
};

struct type
{
    enum type_kind kind;
    int size;
    int align;
    int is_unsigned;

    struct type* base;        /* pointer, array element, function return */
    int array_len;            /* -1 when unknown */

    /* struct / union */
    struct member* members;
    const char* tag;          /* struct/union tag, NULL if anonymous */
    int is_complete;

    /* function */
    struct param* params;
    int has_prototype;
    int is_variadic;
};

int type_is_integer(const struct type* t);
int type_is_float(const struct type* t);
int type_is_arith(const struct type* t);
int type_is_scalar(const struct type* t);
int type_is_record(const struct type* t);  /* struct or union */

/* ------------------------------------------------------------------- AST */

struct obj;

enum node_kind
{
    N_NUM,        /* integer constant: ival */
    N_FNUM,       /* floating constant: fval */
    N_VAR,        /* object or function: var */
    N_ADDR,       /* &lhs */
    N_DEREF,      /* *lhs */
    N_MEMBER,     /* lhs.member (lhs is a struct lvalue) */
    N_CAST,       /* (type) lhs  -- also every implicit conversion */
    N_NEG,
    N_NOT,
    N_BITNOT,
    N_ADD,        /* pointer arithmetic is already scaled by the front-end */
    N_SUB,
    N_MUL,
    N_DIV,
    N_MOD,
    N_SHL,
    N_SHR,
    N_BITAND,
    N_BITOR,
    N_BITXOR,
    N_EQ,
    N_NE,
    N_LT,         /* a > b is built as b < a */
    N_LE,
    N_LOGAND,
    N_LOGOR,
    N_COND,       /* cond ? then : els */
    N_COMMA,
    N_ASSIGN,     /* lhs = rhs, rhs already converted to lhs type */
    N_ASSIGN_OP,  /* lhs op= rhs, also ++ and --. See below. */
    N_CALL,
    N_VA_START,   /* va_start: lhs = address of the va_list, rhs = last parameter */
    N_VA_ARG      /* va_arg: lhs = address of the va_list, value of node type */
};

/*
 * N_ASSIGN_OP:  lhs = (lhs type)((op_type)lhs  op  rhs)
 *   op       one of N_ADD, N_SUB, ...
 *   op_type  type of the computation; rhs already has this type
 *   is_post  result is the old value (x++ / x--)
 * lhs is evaluated only once.
 */

struct node
{
    enum node_kind kind;
    struct type* type;
    const struct token* tok;     /* for diagnostics */

    struct node* lhs;
    struct node* rhs;

    struct node* cond;           /* N_COND */
    struct node* then;
    struct node* els;

    unsigned long long ival;     /* N_NUM */
    double fval;                 /* N_FNUM */
    struct obj* var;             /* N_VAR */
    struct member* member;       /* N_MEMBER */

    enum node_kind op;           /* N_ASSIGN_OP */
    struct type* op_type;
    int is_post;

    struct node** args;          /* N_CALL: already converted */
    int nargs;
};

/* initializer for one scalar inside an object (automatic objects) */
struct init_item
{
    struct init_item* next;
    int offset;
    struct type* type;
    struct member* bitfield;     /* not NULL when storing a bit-field */
    struct node* expr;           /* already converted to type */
};

/* image of a static object: bytes plus addresses to patch */
struct reloc
{
    struct reloc* next;
    int offset;                  /* 8 byte slot inside data */
    const char* label;
    long long addend;
};

enum stmt_kind
{
    S_BLOCK,
    S_EXPR,
    S_IF,
    S_WHILE,
    S_DO,
    S_FOR,
    S_GOTO,
    S_LABEL,
    S_BREAK,
    S_CONTINUE,
    S_RETURN,
    S_INIT        /* initialization of an automatic object */
};

struct stmt
{
    enum stmt_kind kind;
    struct stmt* next;           /* next statement in the block */
    const struct token* tok;     /* first token: file and line for debug info */

    struct stmt* body;           /* S_BLOCK list, loop body, label stmt */
    struct node* expr;           /* S_EXPR, conditions, S_RETURN value */
    struct stmt* then;
    struct stmt* els;
    struct node* init;           /* S_FOR */
    struct node* inc;
    const char* label;           /* S_GOTO, S_LABEL */

    struct obj* var;             /* S_INIT */
    struct init_item* items;
};

struct obj
{
    struct obj* next;
    const char* name;            /* name in the source */
    const struct token* tok;     /* where it is declared, for debug info */
    const char* label;           /* name in the assembly (static objects) */
    struct type* type;

    int is_local;                /* automatic */
    int is_function;
    int is_static;               /* internal linkage */
    int is_defined;              /* has storage in this unit */
    int is_tentative;            /* defined without initializer */
    int is_selectany;            /* __declspec(selectany): the linker keeps one copy */

    /* static objects */
    unsigned char* data;         /* NULL = all zero */
    struct reloc* relocs;

    /* parameters and locals */
    int param_index;             /* -1 when it is not a parameter */
    int offset;                  /* frame offset, set by the back-end */

    /* functions */
    struct obj* params;          /* list linked by next */
    struct obj* locals;          /* every automatic object, linked by next_local */
    struct obj* next_local;
    struct stmt* body;
};

struct program
{
    struct obj* globals;         /* functions and static objects, in order */
    int debug;                   /* -g: write debug information */
    const struct target* target;
};

/* several units can be parsed into the same program; unit makes the
   names of internal-linkage symbols unique */
void parse_program(struct token_list* tokens, struct program* out, int unit, const struct target* target);

/* ------------------------------------------------------------- utilities */

/* growable text */
struct text
{
    char* data;
    int size;
    int capacity;
};

void text_printf(struct text* t, const char* fmt, ...);
void text_append(struct text* t, const char* s, int n);

void* cc_alloc(size_t size);
char* cc_strndup(const char* s, int n);
void cc_error(const char* file, int line, int col, const char* fmt, ...);
void cc_warning(const char* file, int line, int col, const char* fmt, ...);
void cc_error_at(const struct token* tok, const char* fmt, ...);

#endif
