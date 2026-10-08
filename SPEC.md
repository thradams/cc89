# cc89: specification

`cc89` is a minimalist, educational C compiler. It is a project separate from cake:
it does not use cake's code and is not used by it.

## 0. Goal

- **Accept cake's output.** cake generates simplified C, and cc89 compiles that C.
  This way cc89 fits into cake's pipeline the same way another compiler
  (cl, gcc, clang) would:

  ```
  source.c → cake → output.c → cc89 → output.exe
  ```

  cc89 generates the `.exe` by itself. It does not call an external assembler or linker.

- **Be minimalist.** The accepted language is only what cake generates. Everything cake
  already resolves beforehand (preprocessor, typedef, enum, sizeof...) is left out.
- **Be educational in two parts:** **machine code generation** (section 9) and
  **building the executable** (the linker, section 10). In these parts, clarity comes before
  performance and the quality of the generated code. The rest (lexer, parser, types) only
  needs to be correct and simple.

Targets, in implementation order:

1. **Windows x64**, the first target
2. Linux x64
3. macOS arm64

The language is a subset of ISO/IEC 9899:1990 (C89), with one extension: `long long`.
References in brackets point to sections of the standard.

---

## 1. What is left out

| Removed | Note |
|---|---|
| Preprocessor | There are no macros, `#include`, `#define` or `#if`. **But** `#pragma` and `#line` are accepted (see 2.2). |
| Line splicing (`\` + newline) | Does not exist. A `\` at the end of a line is an error. |
| Adjacent string concatenation | `"a" "b"` is an error. |
| Trigraphs and digraphs | Do not exist. |
| `typedef` | There are never user-defined type names, and the grammar does not need the *lexer hack*. |
| `enum` | Does not exist. |
| `sizeof` | Does not exist. cake has already replaced each `sizeof` with a constant. |
| `switch`, `case`, `default` | Left out in the first version. cake can generate `if`/`goto`. |
| `auto` | Does not exist. A local variable with no storage class is already automatic. |
| Array without size | Every declared array has an explicit size: `int a[3] = {1,2,3};`. `int a[] = ...` and `extern int a[];` are errors. |
| Struct or union defined outside file scope | `struct`/`union` definitions only appear at file scope. Inside functions, `struct x` is only **used**. |
| `//` comments | Do not exist (C89). `/* */` comments exist. |

`typedef`, `enum`, `sizeof`, `switch`, `case`, `default` and `auto` are not keywords.
If cake generates one of them, the result is a syntax error with a clear message.

---

## 2. Lexical elements

### 2.1 Tokens

```
token: keyword | identifier | constant | string-literal | punctuator
```

Uses *maximal munch*: `a+++b` becomes `a ++ + b`. Whitespace and `/* */` comments
separate tokens and produce nothing.

**Keywords (23)**

```
break    char     const    continue do       double   else     extern
float    for      goto     if       int      long     register return
short    signed   static   struct   union    unsigned void     volatile while
```

**Identifiers**: `[A-Za-z_][A-Za-z0-9_]*`, with no length limit.

**Integer constants**

```
decimal:  [1-9][0-9]*
octal:    0[0-7]*
hex:      0[xX][0-9a-fA-F]+
suffix:   u, l, ul, lu, ll, ull, llu  (any case; ll must be ll or LL)
```

The type is the first in the list in which the value fits:

| Form | No suffix | `u` | `l` | `ul` | `ll` | `ull` |
|---|---|---|---|---|---|---|
| decimal | int, long, long long | unsigned, unsigned long, unsigned long long | long, long long | unsigned long, unsigned long long | long long | unsigned long long |
| octal/hex | int, unsigned, long, unsigned long, long long, unsigned long long | unsigned, unsigned long, unsigned long long | long, unsigned long, long long, unsigned long long | unsigned long, unsigned long long | long long, unsigned long long | unsigned long long |

**Floating constants**: `1.0`, `.5`, `1e10`, `1.5e-3`. The `f` suffix gives `float`, the
`l` suffix gives `long double`, and no suffix is `double`. There are no hexadecimal floats.

**Character constants**: `'c'`, of type `int`. The escapes are
`\' \" \? \\ \a \b \f \n \r \t \v`, octal `\ooo` and hex `\xhh`. A constant with more than
one character is an error. `L'c'` is also an error, because there are no wide characters.

**String literals**: `"..."`, of type `char[N+1]`, with the same escapes. There is no
`L"..."`.

**Punctuators**

```
[ ] ( ) . -> ++ -- & * + - ~ ! / % << >> < > <= >= == != ^ | && ||
? : = *= /= %= += -= <<= >>= &= ^= |= , { } ; ...
```

### 2.2 `#` lines: `#pragma` and `#line`

> **Note:** cc89 **has no preprocessor**, but it accepts two directives. The lexer
> recognizes them directly, with no preprocessing stage.

A line whose first non-space character is `#` must be one of these:

**`#line`**, used to generate debug information:

```
#line <number>
#line <number> "<file>"
# <number> "<file>" <flags>...      (gcc short form, also accepted)
```

The next line becomes line `<number>` of file `<file>`. This position is used:

- in diagnostics;
- **in the debug information of the generated code**, so that the debugger shows the
  original source, from before cake, and not `output.c` (see 9.6).

**`#pragma`**:

```
#pragma <anything up to the end of the line>
```

The lexer delivers the line as a `PRAGMA` token. It can appear at file scope
or inside a block, where a declaration or a statement would fit. An unknown pragma
produces a warning and is ignored. The list of supported pragmas is open (see 10).

Any other `#` line is an error.

---

## 3. Types

### 3.1 Types and specifiers

| Specifiers (in any order) | Type |
|---|---|
| `void` | void |
| `char` | char |
| `signed char` | signed char |
| `unsigned char` | unsigned char |
| `short`, `short int`, `signed short`, `signed short int` | short |
| `unsigned short`, `unsigned short int` | unsigned short |
| `int`, `signed`, `signed int` | int |
| `unsigned`, `unsigned int` | unsigned int |
| `long`, `long int`, `signed long`, `signed long int` | long |
| `unsigned long`, `unsigned long int` | unsigned long |
| `long long`, `long long int`, `signed long long`, `signed long long int` | long long |
| `unsigned long long`, `unsigned long long int` | unsigned long long |
| `float` | float |
| `double` | double |
| `long double` | long double |
| `struct tag`, `union tag` | struct or union |

From these types, pointers (`T *`), arrays (`T [N]`) and functions (`T (params)`) are derived.

- Any other combination of specifiers is an error.
- **There is no implicit int.** Every declaration has a type, and `static x;` is an error.
- The `const` and `volatile` qualifiers are accepted and checked in assignments.
  `volatile` prevents keeping the value in a register between accesses.

### 3.2 Sizes per target

Values are size/alignment in bytes:

| Type | Windows x64 (LLP64) | Linux x64 (LP64) | macOS arm64 (LP64) |
|---|---|---|---|
| char | 1/1, signed | 1/1, signed | 1/1, signed |
| short | 2/2 | 2/2 | 2/2 |
| int | 4/4 | 4/4 | 4/4 |
| long | **4/4** | 8/8 | 8/8 |
| long long | 8/8 | 8/8 | 8/8 |
| pointer | 8/8 | 8/8 | 8/8 |
| float | 4/4 | 4/4 | 4/4 |
| double | 8/8 | 8/8 | 8/8 |
| long double | **8/8 (= double)** | 16/16 (x87) | **8/8 (= double)** |

Pointer subtraction has type `long long` on Windows and `long` on the other targets.

On Linux, `long double` may initially be treated as `double`, with a warning, to avoid
requiring x87 code in the first version.

### 3.3 Struct and union

- **The definition only appears at file scope**: `struct s { ... };`.
- There can be a forward declaration, also at file scope: `struct s;`.
- Inside functions and parameters only `struct s` is used, never `struct s { ... }`.
- There is a single, global tag namespace, shared by struct and union.
  Defining the same tag twice is an error.
- A member cannot have an incomplete type or a function type.
- Bit-fields are left out in the first version (see 10).
- The layout follows member order, with padding to align each one. The total size
  is rounded up to the struct's alignment. This is compatible with each target's ABI.
- `#pragma pack` is left out in the first version (see 10).
- Assigning, passing and returning structs by value is allowed.

### 3.4 Incomplete types

`void` and `struct s` before its definition are incomplete. Creating an object,
accessing a member or doing pointer arithmetic with them is not allowed.

### 3.5 Compatibility

Two types are compatible when they are the same basic type with the same qualifiers,
pointers to compatible types, arrays of the same size with compatible elements,
functions with the same return and the same parameters, or the same tag.

---

## 4. Expressions

### 4.1 Grammar (precedence from highest to lowest)

```
primary:        identifier | constant | string-literal | ( expression )
postfix:        primary
                | postfix [ expression ]
                | postfix ( argument-list? )
                | postfix . identifier
                | postfix -> identifier
                | postfix ++ | postfix --
unary:          postfix
                | ++ unary | -- unary
                | ( & | * | + | - | ~ | ! ) cast
cast:           unary | ( type-name ) cast
multiplicative: cast (( * | / | % ) cast)*
additive:       multiplicative (( + | - ) multiplicative)*
shift:          additive (( << | >> ) additive)*
relational:     shift (( < | > | <= | >= ) shift)*
equality:       relational (( == | != ) relational)*
bit-and:        equality ( & equality )*
bit-xor:        bit-and ( ^ bit-and )*
bit-or:         bit-xor ( | bit-xor )*
logical-and:    bit-or ( && bit-or )*
logical-or:     logical-and ( || logical-and )*
conditional:    logical-or ( ? expression : conditional )?
assignment:     conditional | unary assign-op assignment
expression:     assignment ( , assignment )*
```

**Cast or parenthesized expression?** After `(`, if the next token is a
type keyword, `const`, `volatile`, `struct` or `union`, it is a cast. Otherwise, it is a
parenthesized expression. Since there is no typedef, one token of lookahead is enough.

### 4.2 Conversions

**Integer promotion**: `char`, `short` and their unsigned versions become `int`.

**Usual arithmetic conversions**. The rank is int < long < long long.

1. If an operand is `long double`, `double` or `float`, in that order of priority, the
   other is converted to that type.
2. Otherwise, integer promotion is applied to both.
3. If both have the same type, that is the result.
4. If both have the same signedness, the result is the one with higher rank.
5. If the unsigned one has greater or equal rank, the result is the unsigned one.
6. If the signed one can represent all values of the unsigned one, the result is the signed one.
7. Otherwise, the result is the unsigned version of the signed one.

The result depends on the target. For example, `long` with `unsigned int` gives `unsigned long`
on Windows and `long` on Linux and macOS.

**Decay**: an array becomes a pointer to its first element and a function becomes a
pointer to function. The exception is the operand of `&`.

**Null pointer**: the integer constant `0`, with or without a cast to `void *`.

### 4.3 Operators

| Operator | Operands | Result |
|---|---|---|
| `a[i]` | a pointer to a complete object and an integer | lvalue |
| `f(...)` | function or pointer to function | return type |
| `.` `->` | struct/union, or pointer to struct/union | member lvalue |
| `++` `--` | modifiable lvalue, arithmetic or pointer | — |
| `&` | lvalue that is not `register`, or function | pointer |
| `*` | pointer | lvalue |
| unary `+` `-` | arithmetic | promoted |
| `~` | integer | promoted |
| `!` | scalar | int |
| cast | scalar to scalar, or any type to `void` | not an lvalue |
| `*` `/` | arithmetic | usual conversion |
| `%` | integer | usual conversion |
| `+` | two arithmetic, or pointer and integer | — |
| `-` | two arithmetic, pointer and integer, or two compatible pointers | — |
| `<<` `>>` | integer | type of the promoted left operand |
| `<` `>` `<=` `>=` | two arithmetic or two compatible pointers | int |
| `==` `!=` | same as above, plus pointer with `void *` and pointer with null | int |
| `&` `^` `\|` | integer | usual conversion |
| `&&` `\|\|` | scalar | int, short-circuit |
| `?:` | the condition is scalar and the branches follow the rules of [6.3.15] | — |
| `=` | modifiable lvalue on the left and assignable value on the right (see 4.4) | type of the left |
| `op=` | `E1 = E1 op E2`, but E1 is evaluated only once | — |
| `,` | any | the right operand |

### 4.4 Assignment

The right value is assignable to the left object when one of these holds:

- both are arithmetic;
- they are the same struct or union;
- they are compatible pointers, and the left pointee has all the qualifiers of the
  right pointee;
- one is `void *` and the other is a pointer to object;
- the left is a pointer and the right is null.

The same rule applies to arguments, `return` and initializers.

### 4.5 Calls

- **Every called function must have a visible prototype.** There is no implicit
  declaration nor function without a prototype. `int f();` is treated as an error, and the
  correct form is `int f(void);`.
- The number of arguments must match, except when there is `...`. Each argument is
  converted as in an assignment.
- Arguments that fall into `...` get the default promotions: integer promotion, and
  `float` becomes `double`.
- Arguments are evaluated left to right. The standard does not fix the order, and
  cc89 chooses this one to be predictable.

### 4.6 Constant expressions

**Integer constant**: integer and character literals, and arithmetic,
bitwise, relational, logical, `?:` operators and casts to integer applied to them.

**Used in**: array sizes and static initializers.

**Static initializer**: arithmetic constant, string, `&static_object`, name of a
function or static array, optionally plus an integer constant, or null.

---

## 5. Declarations

### 5.1 Grammar

```
declaration:     specifiers init-declarator-list? ;
specifiers:      ( storage-class | type-specifier | type-qualifier )+
storage-class:   static | extern | register
type-specifier:  void | char | short | int | long | float | double
                 | signed | unsigned | struct-or-union-ref
struct-or-union-ref:     ( struct | union ) identifier
struct-or-union-def:     ( struct | union ) identifier { member-declaration+ } ;
member-declaration:      specifiers declarator ( , declarator )* ;
init-declarator: declarator ( = initializer )?
declarator:      pointer? direct-declarator
pointer:         ( * type-qualifier* )+
direct-declarator:
                 identifier
                 | ( declarator )
                 | direct-declarator [ constant-expression ]   (size required)
                 | direct-declarator ( parameter-list )
parameter-list:  void | parameter ( , parameter )* ( , ... )?
parameter:       specifiers declarator
type-name:       specifiers abstract-declarator?
abstract-declarator:
                 pointer | pointer? direct-abstract-declarator
direct-abstract-declarator:
                 ( abstract-declarator )
                 | direct-abstract-declarator? [ constant-expression ]
                 | direct-abstract-declarator? ( parameter-list )
initializer:     assignment | { initializer ( , initializer )* ,? }
```

`struct-or-union-def` is only accepted as an external declaration (see 7).

### 5.2 Rules

- At most one storage class per declaration. In parameters, the only one accepted is
  `register`.
- An array's size is an integer constant greater than 0 and **always** appears.
- A parameter declared as `T a[N]` becomes `T *a`, as in C.
- A function cannot return an array or a function. There are no arrays of functions.
- Every parameter has a name in a function definition.
- There are no K&R-style function definitions.
- Redeclaring in the same scope is only allowed when there is external or internal linkage and the
  types are compatible.
- In block scope, a new name hides the one with the same name from the outer scope.

### 5.3 Linkage and storage

- At file scope, `static` gives internal linkage and `extern`, or no storage class, gives
  external linkage.
- In a block, `static` gives static duration with no linkage, `extern` refers to an external
  name and no storage class gives an automatic variable.
- **Tentative definition**: a declaration `int x;` at file scope becomes `int x = 0;`
  if there is no other definition in the unit.

### 5.4 Initialization

- A static object without an initializer gets zero.
- A static object only accepts constant initializers (see 4.6).
- An automatic scalar accepts any assignable expression. An automatic struct
  accepts an expression of the same type.
- An automatic array or struct accepts a brace-enclosed list, but only with constants,
  as C89 requires.
- There are no designators. Braces follow member order and can be nested.
  The first version requires full braces, with no omissions.
- A union initializes only its first member.
- Missing trailing members get zero. Extra initializers are an error.
- A `char` array can receive a string. If the string, with its `\0`, fits in the declared
  size, the rest is filled with zero. If exactly the `\0` does not fit, it is
  dropped. A string larger than the array is an error.

---

## 6. Statements

```
statement:  identifier : statement
            | compound
            | expression? ;
            | if ( expression ) statement ( else statement )?
            | while ( expression ) statement
            | do statement while ( expression ) ;
            | for ( expression? ; expression? ; expression? ) statement
            | goto identifier ;
            | continue ;
            | break ;
            | return expression? ;
            | PRAGMA
compound:   { declaration* statement* }
```

- Declarations come before statements in a block, as in C89.
- There are no declarations in `for`.
- The condition of `if`, `while`, `do` and `for` must be scalar.
- `break` is only valid inside a loop, and so is `continue`.
- Labels are unique in the function. Using a label that does not exist is an error.
- `return expr;` in a `void` function is an error, and so is `return;` in a non-`void` function.
- `else` binds to the nearest `if`.

---

## 7. Translation unit

```
translation-unit:     external-declaration*
external-declaration: function-definition | declaration | struct-or-union-def | PRAGMA
function-definition:  specifiers declarator compound
```

- An empty unit is accepted.
- The return type cannot be an array or a function.
- `main` has no special rule.

---

## 8. Diagnostics

The format is `file:line:column: (error|warning): message`. The position honors `#line`.

- The compiler stops at the **first error**. This is simpler, because
  it requires no error recovery.
- Warnings: unknown pragma and `long double` treated as `double`.
- The message says what was expected: `expected ';' after expression`.

---

## 9. Code generation

This is the part that matters most for the educational goal.

### 9.1 Principles

1. **Clarity over efficiency.** The generated code can be slow. The generator's code
   must be obvious.
2. **One function per construct.** `gen_expr`, `gen_stmt`, `gen_if`, `gen_while`,
   `gen_call` etc. Each one handles one case and is commented with the assembly pattern
   it produces.
3. **Stack machine over real registers.** Every expression leaves its result in a fixed
   register: `rax` for integers and pointers, `xmm0` for floating point.
   One binary operand is saved on the stack (`push`) while the other is computed.
   There is no register allocation.
4. **All local variables live on the stack**, at a fixed offset from `rbp`.
   `register` is accepted and ignored.
5. **No optimization** in the first version.
6. **Separation by target.** What is common (walking the AST, labels, layout) is in one
   file. What depends on the target (instructions, calling convention, executable format) is in one
   file per target: `gen_win64.c`, `gen_sysv64.c`, `gen_arm64.c`.


### 9.2 Generator output

The generator writes **machine code bytes** directly into a buffer for the `.text` section.
There is no text assembly stage.

To keep this part readable, writing is split into three layers:

1. **`gen_*`**: walks the AST and decides the instructions, for example "load the local
   variable at offset -8 into `rax`".
2. **`emit_*`**: one function per x64 instruction, such as `emit_mov_reg_mem(RAX, RBP, -8)`.
   Each one has a comment with the assembly instruction and its encoding (REX,
   opcode, ModRM, SIB, displacement, immediate).
3. **Section buffer**: a growing byte vector, with `emit_u8`, `emit_u32` etc.

**Listing (`-S`)**: each `emit_*` can also write the corresponding assembly line,
with the address and bytes, to a `.lst` file:

```
00000010  48 8B 45 F8        mov rax, [rbp-8]        ; source.c:12
```

This is how the student sees what was generated, and it makes an assembler unnecessary.

**Encoding**: only the small set of instructions the generator uses, always in the same
form, even when a shorter form exists. For example, every memory displacement
uses 32 bits. This keeps the encoder simple and free of special cases. The instruction list
lives in a single file, `x64_encode.c`.

**Jumps**: every jump uses a 32-bit displacement (`jmp rel32`, `jcc rel32`). Statement
labels (`if`, loops, `goto`) are resolved at the end of each function: the generator records
each pending jump and fills in the displacement when the label is defined.

**Symbol references** (function calls, address of a global, string) become
**relocations** inside the compiler: `{section, offset, symbol, type}`. The linker
(section 10) resolves them.

### 9.3 Windows x64: calling convention (Microsoft x64)

- The first 4 arguments go in `rcx`, `rdx`, `r8` and `r9`, or in `xmm0` to `xmm3` if
  they are floating point. The argument's position decides the register, not its type.
- The caller reserves 32 bytes of *shadow space* on the stack, even if there are fewer than 4
  arguments.
- Arguments from the 5th on go on the stack, after the shadow space.
- `rsp` is 16-byte aligned at the moment of the `call`.
- The return value comes in `rax`, or in `xmm0` if it is floating point.
- Structs of 1, 2, 4 or 8 bytes are passed and returned as an integer. Others are
  passed by pointer to a copy, and the return uses a hidden pointer in `rcx`.
- In variadic functions, a floating-point value goes **also** in the corresponding
  integer register.
- The volatile registers are `rax rcx rdx r8-r11 xmm0-xmm5`. Since the generator does not
  keep values in registers between statements, this only matters for pushed
  values.

### 9.4 Stack frame (Windows x64)

```
        arguments 5+                  [rbp + 48 ...]
        shadow space (caller's)       [rbp + 16 .. rbp + 47]
        return address                [rbp + 8]
rbp →   saved rbp                     [rbp]
        local variables and temporaries [rbp - N]
        outgoing argument area        (shadow space + arguments of calls made)
rsp →
```

In the prologue, the 4 register parameters are copied to the shadow space. This way
**all** parameters live in memory, at an address relative to `rbp`, and are treated like
any local variable.

Every function uses the same prologue and the same epilogue:

```
push rbp
mov  rbp, rsp
sub  rsp, N          ; N multiple of 16, computed at the end of the function and patched in later
...
mov  rsp, rbp
pop  rbp
ret
```

**Unwind info**: Windows x64 requires, for each non-leaf function, an entry in the
`.pdata` table and an `UNWIND_INFO` in `.xdata`. Without it, exceptions and some debuggers
fail. Since the prologue is always the same, the `UNWIND_INFO` is also always the same; only
`N` changes. The generator writes one per function.

### 9.5 Linux x64 and macOS arm64

These are the second and third stages. The detailed specification is left for when their
turn comes: SysV AMD64 with ELF and Apple's AAPCS64 with Mach-O. The `gen_*` layers are
shared. What changes is `emit_*` (on arm64), the calling convention and the executable
format.

### 9.6 Code generation tests

- Each test is a small C program that returns a value, or prints something, and is
  compared with the expected result.
- Encoder tests: the listing of each `emit_*` is compared with the disassembly from
  `dumpbin /disasm` or `llvm-objdump`.
- Pipeline tests: `source.c → cake → cc89 → exe` compared with
  `source.c → cl → exe`.

---

## 10. Building the executable (linker), Windows

This is the second educational part. cc89 generates the PE32+ file (`.exe`) directly.

### 10.1 Model

- **There are no `.obj` or `.lib` files.** cc89 receives all `.c` files at once
  (`cc89 a.c b.c -o prog.exe`), compiles each one into in-memory sections and links everything at the
  end.
- Each translation unit produces sections (`.text`, `.data`, `.rdata`, `.bss`), a
  symbol table and a list of relocations. The linker merges everything.
- **External functions come only from DLLs.** The linker imports, for example, `printf` from
  `ucrtbase.dll` and `ExitProcess` from `kernel32.dll`. There is no static library or
  static CRT.

### 10.2 Linker steps

Each step is a commented function, in the order it runs:

1. **Merge sections.** Concatenates same-named sections from all units, each one
   aligned. Records where each unit starts inside the final section.
2. **Resolve symbols.** Builds the global table. `static` symbols are visible only in
   their own unit. A symbol defined twice is an error. Tentative definitions from
   different units become one, in `.bss`.
3. **Resolve imports.** Each symbol that is still undefined is looked up in the known
   DLLs (see 10.4). If it is in none of them, it is an error:
   `undefined symbol: foo`.
4. **Generate the import tables** (`.idata`): Import Directory, Import Lookup Table,
   Import Address Table (IAT) and the names. Each imported function gets a *thunk* in
   `.text`, `jmp [rip+IAT]`, so that the call in the generated code is always a plain
   `call rel32`.
5. **Assign addresses.** Chooses the RVA of each section, aligned to `SectionAlignment`
   (0x1000), and the file position, aligned to `FileAlignment` (0x200).
6. **Apply relocations.** Fills in each pending reference. With all code
   RIP-relative, there are only two kinds: `REL32`, for calls and data accesses, and
   `ADDR64`, for pointers stored in data, such as `int *p = &x;`.
7. **Write the file**: DOS header and *stub*, `PE\0\0` signature, COFF File Header,
   PE32+ Optional Header, Data Directories (Import, Exception/`.pdata`, IAT, Debug),
   section table and finally the section contents.

### 10.3 Simplifying decisions

- **Fixed base** at `ImageBase = 0x140000000`, without ASLR (`DYNAMIC_BASE` off). With
  this, there is no need to generate the `.reloc` table. It can come later, as an exercise.
- Console subsystem (`IMAGE_SUBSYSTEM_WINDOWS_CUI`).
- **Entry point**: the linker generates a startup function, `_cc89_start`, that gets
  `argc`/`argv`, calls `main` and passes the return value to `exit`. The UCRT functions it
  uses are `__p___argc`, `__p___argv` and `_initterm` if needed, plus `exit`.
- Fixed sections: `.text`, `.rdata` (constants, strings and imports), `.data`, `.bss`,
  `.pdata` and `.xdata`.

### 10.4 Which DLLs

The symbol must come from some DLL, and the linker must know which one. Options:

- **(a)** The linker opens the DLLs in a list (by default `kernel32.dll` and
  `ucrtbase.dll`, plus those given with `-l`), reads the **export table** of each one and
  looks up the symbol. Reading the export table is also PE, so it reuses the linker's
  code and is educational.
- **(b)** A built-in table with `name → DLL` for the libc functions.

Suggestion: **(a)**.

### 10.5 Linker tests

- The generated `.exe` must pass `dumpbin /headers /imports` without warnings and run.
- Tests with several units, `static` with the same name in two units, duplicate
  symbol and undefined symbol.

---

## 11. Debug information

The goal is to debug the **original source**, from before cake, using the positions that
`#line` provides. For each statement, the generator records the pair
`(address in .text, file:line)`.

Since there is no external linker, cc89 itself must write the debug format.
On Windows, Visual Studio and WinDbg only read **PDB**, a large and poorly
documented format. Options:

- **(a)** Write a minimal PDB, with only lines and functions. This is laborious, and the
  reference is LLVM's code.
- **(b)** Write DWARF inside the PE. gdb and Windows lldb understand it, but Visual
  Studio does not.
- **(c)** First version without debug info, only with the `.lst` listing (9.2), which already shows
  the source line of each instruction.

Suggestion: **(c)** in the first version, then **(a)**.

---

## 12. Open questions

1. **Debug format on Windows** (section 11).
2. **How to find the DLLs** (section 10.4).
3. **What exactly cake generates.** It must be confirmed against cake's real output that it
   never contains `sizeof`, `enum`, `typedef`, `switch`, concatenated strings, arrays without
   size or local structs. It must also be confirmed whether it uses bit-fields and
   which `#pragma`s it emits.
4. **List of pragmas.** Which ones does cc89 implement? The candidate is `pack`, which the Windows
   headers use.
5. **Bit-fields.** They go in if cake generates them.
6. **Omitted braces in initializers.** They go in if cake generates them.
7. **CRT globals and functions that are macros or inline in the headers**, such as `errno`,
   `stdin` and `stdout`. After cake they already come expanded, for example `stdout` becomes
   `__acrt_iob_func(1)`, which is a UCRT function. It must be confirmed that everything that
   remains is a function importable from a DLL.
