# cc89

Minimal C89 compiler: **source → cc89 → executable**, with no external assembler or linker.
The input is cake's output. See [SPEC.md](SPEC.md).

| Target | cake | cc89 | Output |
|---|---|---|---|
| Windows x64 | `-target=msvc-win-x64` | `-target=win64` (default on Windows) | PE (.exe), ucrtbase/msvcrt + kernel32 |
| Linux x64 | `-target=gcc-linux-x64` | `-target=linux-x64` (default on Linux) | dynamic ELF, libc.so.6 + libm.so.6 |
| macOS arm64 | `-target=clang-macos-arm64` | `-target=macos-arm64` | signed Mach-O (ad-hoc), libSystem |

```
cc89 a.c [b.c ...] [-o output] [-target=win64|linux-x64|macos-arm64] [-l lib] [-S] [-g] [-map]
```

- `-g` emits debug information: on Linux, DWARF 4 inside the ELF (gdb); on Windows, a `.pdb` next to the `.exe`
  (Visual Studio, WinDbg, cdb).

- `-S` also writes the assembly (`output.s`), to read what was generated.
- `-map` writes `output.map` with `address symbol` (Linux), useful to locate a crash.
- `-l` adds a DLL (Windows) or a shared library (Linux, e.g. `libpthread.so.0`).
- cc89 runs on Windows and Linux, and either one generates for any target.
  On Windows, exported names are read from the DLLs themselves, so no `.lib` is needed.
  On Linux, names are resolved by the loader (`ld-linux`) when the program starts.

## Pipeline

| File | Stage |
|---|---|
| `lexer.c` | text → tokens |
| `parser.c`, `type.c`, `target.c` | tokens → typed AST (front end; sizes and layout depend on the target) |
| `cc89.h` | contract between front end and back ends |
| `backend_x64.c` | AST → text assembly (stack machine, shared by both x64 targets) |
| `abi_win64.c` | Microsoft x64 calling convention |
| `abi_sysv.c` | System V calling convention (struct classification, `va_list`) |
| `asm_x64.c` | assembly → x64 bytes |
| `pe.c` | Windows linker: DLL imports → PE32+ |
| `backend_arm64.c` | AST → AArch64 assembly (Apple ABI: variadics on the stack) |
| `asm_arm64.c` | assembly → AArch64 bytes |
| `macho.c` | macOS linker: stubs, GOT, chained fixups, ad-hoc signature → Mach-O |
| `elf.c` | Linux linker: GOT, thunks, dynamic section → ELF64; with `-g`, also `.debug_line`, `.debug_frame` and `.symtab` |
| `dwarf.c` | `-g` on Linux: `.debug_info` and `.debug_abbrev` (functions, parameters, variables, types). Independent of the instruction set |
| `codeview.c` | `-g` on Windows: CodeView types (`.debug$T`) and symbols (`.debug$S`), as in an MSVC `.obj` |
| `pdb.c` | `-g` on Windows: writes the `.pdb` (MSF file with type, module, line and hash streams) |

## Differences from the SPEC (because of cake's output)

- There is no `switch`, character constants, string concatenation or line splicing
  with `\`. The exception is `\`+newline **inside strings**, which appears in cake's output.
- `const`/`volatile` are read and ignored. Only errors that prevent code generation are reported.
- `L"..."` is accepted (UTF-16 on Windows, UTF-32 on Linux).
- Windows: `__cdecl`, `__declspec(...)` (with `selectany`), `__int64` and `__inline` are accepted.
  There are also `__va_start` and the `_InterlockedCompareExchange*` intrinsics.
  `wmain` is accepted as the entry point.
- Linux: `__builtin_va_list/va_start/va_arg/va_end/va_copy`, `__builtin_inf/nan`,
  and `__builtin_floor` etc., which become libm calls.
- Not supported: SEH (`__try/__except`) and x87 `long double` on Linux (an error if used in code;
  declarations in headers are fine).

## Build and tests

```
build.bat                (Windows)    sh build.sh             (Linux)
sh tests/run.sh          (Windows; with cdb installed, also tests the .pdb)    sh tests/linux/run.sh   (Linux or WSL)
```

sqlite3 + shell, passed through cake and compiled by cc89, give the same output as the gcc
build, on both Windows and Linux. cake compiled by cc89 (Windows) produces
output identical to the original cake.

## macOS arm64

`macho.c` generates the executable without `ld` or `codesign`:

- segments `__PAGEZERO`, `__TEXT` (`__text`), `__DATA` (`__data`, `__got`), `__LINKEDIT`; 16 KB pages, `MH_PIE`;
- no startup code: `LC_MAIN` points to `main`, and dyld calls `exit` with the result;
- each imported function is called through a stub (`adrp x16` / `ldr x16` / `br x16`) that reads `__got`;
- pointers in `__DATA` (GOT and addresses in static objects) are `LC_DYLD_CHAINED_FIXUPS`
  (`DYLD_CHAINED_PTR_64_OFFSET`); imported names get a `_` prefix (`_printf`);
- `-l /path/lib.dylib` adds an `LC_LOAD_DYLIB`; in that case symbols are looked up in all
  libraries (flat lookup);
- ad-hoc signature (`LC_CODE_SIGNATURE`): a CodeDirectory with the SHA-256 of each 4 KB page.

Not yet tested on a Mac: `otool -l`, `codesign -v` and running `tests/linux/abi.c` (generated with
`cake -target=clang-macos-arm64`) are the next step.

`-g` on macOS: the executable carries an `LC_UUID`, and the DWARF (`__debug_info`, `__debug_abbrev`,
`__debug_line`, `__debug_aranges`) goes to `out.dSYM/Contents/Resources/DWARF/out`, an `MH_DSYM`
with the same UUID, as `dsymutil` does. lldb finds the `.dSYM` next to the executable.

## Debug (`-g`)

### Linux (DWARF, gdb)

For lines to point to the original source, generate with `cake -line-directives`:

```
cake -target=gcc-linux-x64 -line-directives main.c -o out/main.c
cc89 -target=linux-x64 -g out/main.c -o main
gdb ./main
```

| Part | Generated by | What it is for in gdb |
|---|---|---|
| `.debug_line` | back end (`.loc` on each statement) → assembler (lines) → linker | `break file:line`, `next`/`step`, `list` |
| `.debug_info`/`.debug_abbrev` | `dwarf.c` | `print`, `info locals/args`, types (structs, bit-fields, arrays, pointers) |
| `.debug_frame` | linker (the prologue is always `push rbp; mov rbp, rsp`) | `bt`, `up`, `finish` |
| `.symtab` + section headers | linker | function names, `readelf`, `objdump` |

Limitations: there are no lexical blocks, so a variable from an inner block shows up as
a local of the whole function. `-g` also does not change the code: it is the same as without `-g`.

### Windows (PDB: Visual Studio, WinDbg, cdb)

```
cake -target=msvc-win-x64 -line-directives main.c -o out/main.c
cc89 -g out/main.c -o main.exe          (also generates main.pdb)
cdb -lines main.exe                     (or open main.exe in Visual Studio / WinDbg)
```

| Part | Generated by | What it is for |
|---|---|---|
| `.debug$T` → TPI stream | `codeview.c` | types: structs (forward refs resolved by name via hash), bit-fields, pointers, arrays, functions |
| `.debug$S` → module stream | `codeview.c` | `S_GPROC32` + `S_REGREL32` (parameters and locals relative to `rbp`), `S_GDATA32` |
| C13 lines | assembler (`.loc`) → `pdb.c` | line breakpoints, step, file:line in `k` |
| globals/publics (GSI) | `pdb.c` | `bp name`, `?? global`, `x module!*` |
| `.pdata`/`.xdata` in the PE | `pe.c` | stack walking (`k`), also without `-g` |
| debug directory (RSDS) in the PE | `pe.c` | links the `.exe` to the `.pdb` (GUID + age) |

The format follows LLVM's PDB writer and Pascal Beyer's documentation
(github.com/PascalBeyer/PDB-Documentation). `llvm-pdbutil dump -all x.pdb` (ships with Visual
Studio) shows everything that was written. A `bp function` stops before the prologue, as with MSVC;
after one step, the parameters show correctly.

Next: debugging on macOS.
