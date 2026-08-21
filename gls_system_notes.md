# Gunlok GLS/GSH Script System - Reverse Engineering Notes

## Overview

GLS ("GunLok Script") files describe all static game data: shapes, model hierarchies,
lights, particle generators, projectiles, characters, roles, ammo, maps, camera tracks
and resource directories. `.gls` files are level entry points, `.gsh` files are shared
includes (`#include`), `.gcs` files are console/trigger scripts executed separately by
the console system (see trigger_system_notes.md).

The parser is a classic **flex + yacc** pair compiled into the binary:

- `GSHTokenize` @ `0x00474920` - flex DFA lexer (keywords are compiled into the DFA)
- `ParseGSH` @ `0x00478400` - yacc LALR parser; all grammar actions live in one big
  reduce-rule switch

The parser generator is **Berkeley yacc**, and that is not an inference from shape: byacc's own
SCCS stamp is linked in among the parser tables at 0x006a3d54, 36 bytes reading
`@(#)yaccpar	1.4 (Berkeley) 02/25/90`.

### The lexer and parser tables

Both are **table-driven**, and the sixteen tables are now defined and named in the database. Every
boundary below is measured by exact adjacency; the two that are pinned by a *foreign* object rather
than by symmetry are called out, because both corrected an earlier proposal.

`GSHTokenize`'s seven, contiguous in `.rdata` — 5958 states, 55 character equivalence classes:

| Address | Type | Name | Role |
|---|---|---|---|
| 0x00652938 | `short[5960]` | `GshLex_yy_accept` | per-state accepting rule |
| 0x006557c8 | `byte[256]` | `GshLex_yy_ec` | char -> equivalence class, values 0..55. Measured directly: `MOV BL,byte ptr [EAX + 0x6557c8]` |
| 0x006558c8 | `byte[56]` | `GshLex_yy_meta` | meta-equivalence class, `nclasses+1` entries, values 1..5 |
| 0x00655900 | `short[5968]` | `GshLex_yy_base` | per-state base offset |
| 0x006587a0 | `short[5968]` | `GshLex_yy_def` | default/next state; jam values 5958 / 5959 |
| 0x0065b640 | `short[7396]` | `GshLex_yy_nxt` | transition table |
| 0x0065f008 | `short[7394]` | `GshLex_yy_chk` | owner-state table |

then the string pool at 0x006629cc. `yy_chk`'s **base** is pinned decisively rather than by
symmetry: for every index *i* in 1..7299, `yy_chk[i]` names a state *s* with
`yy_base[s] <= i <= yy_base[s]+55` — **7299/7299 with zero violations at 0x0065f008**, against 5
violations at 0x0065f006 and 11 at 0x0065f00a. Its *length* then follows from the string pool,
giving 7394 — two short of `yy_nxt`, the one asymmetry in the set.

`ParseGSH`'s nine, contiguous in `.data`, in byacc's canonical emission order — ~288 states, 56
nonterminals, 246 rules, index bound `CMP EBX,0x351`:

| Address | Type | Name |
|---|---|---|
| 0x006a3b68 | `short[246]` | `GshParse_yylhs` (starts `-1, 0, 0, 0x23, 0x23, 0x24 ...`) |
| 0x006a3d54 | `char[36]` | `GshParse_yaccpar_sccsid` — **the byacc version stamp, linked between two tables** |
| 0x006a3d78 | `short[248]` | `GshParse_yylen` (rule lengths `2,2,2,2,0,1,1,1,3,3,3...`) |
| 0x006a3f68 | `short[288]` | `GshParse_yydefred` |
| 0x006a41a8 | `short[56]` | `GshParse_yydgoto` |
| 0x006a4218 | `short[288]` | `GshParse_yysindex` |
| 0x006a4458 | `short[288]` | `GshParse_yyrindex` |
| 0x006a4698 | `short[56]` | `GshParse_yygindex` |
| 0x006a4708 | `short[852]` | `GshParse_yytable` |
| 0x006a4db0 | `short[852]` | `GshParse_yycheck` |

`yylhs` is `short[246]`, **not** the 264 first proposed: the SCCS stamp begins at 0x006a3d54 and
hard-bounds it. (byacc normally emits `yylhs` and `yylen` at equal length and `yylen` here is 248,
so the 2-entry difference is real and unexplained — but the string is not movable.) `yycheck`'s
extent is confirmed rather than proposed: `0x006a4db0 + 852*2 = 0x006a5458` lands exactly on the
start of the next object. `yytable + 850*2 = 0x006a4dac`, immediately preceding `yycheck`, which
reconciles the `CMP 0x351` bound against the layout independently.

**These tables are a classifier trap, and they have already caught one sweep.** Every dword in them
has the byte shape `XX 00 YY 00` — two small 16-bit values that read as a plausible `.text` pointer
when paired. A pointer-table pass deferred ten runs as "unresolved switch jump tables"
(0x0065b6f8[8], 0x0065b7ac[19], 0x0065eba4[11], 0x0065f2f8[5], 0x0065f310[6], 0x0065f330[5],
0x006a4144[5], 0x006a4170[5], 0x006a4850[8], 0x006a4898[7]); **none is a jump table** and no
override should ever be applied to one. A census of the two regions found **159 dwords that read as
mid-`.text` pointers** — 114 in the lexer tables (0.9% of 12,980 dwords) and 45 in the parser tables
(2.8% of 1,595); the ten are just the ones long enough to pass a run-length threshold. Two tests
settle it, either cheaper than finding the dispatch:

1. **Byte shape** — reject a dword whose bytes are `XX 00 YY 00` with `XX,YY < 0x80`. A genuine
   `.text` pointer here (0x00401000-0x0064cfff) always has a nonzero `byte[1]` or `byte[2]`; a pair
   of small `short`s never does.
2. **Decisive and cheap** — require the candidate base to appear as a `disp32` somewhere in `.text`.
   A jump table that nothing dispatches through is not a jump table, and this is **immune to the
   undisassembled-code trap**: it does not depend on the referencing code being disassembled. The
   base of each of the ten appears nowhere in `.text`/`.rdata`/`.data` as a 4-byte LE constant.

Two cheaper tells were available all along: four of the ten sit in `.data`, and MSVC never puts a
jump table in a writable section; and 0x0065b7ac's 19 "targets" land in **eight unrelated
functions**. Note also that `CLAUDE.md`'s documented `0x00630063` false positive at
0x006a4b70-0x006a4b97 is **inside `GshParse_yytable`** — `0x0063` there is parser state 99,
repeated — so that trap's worked example is not an isolated curiosity but a slice of this same
object. The real arrays are read with `MOVSX`/`MOVZX reg, word ptr [idx*0x2 + base]` — **scale 2,
16-bit** — and never by an indirect `JMP`.

Defining these correctly pulls in **zero** new code: a `short[]` array creates no references into
`.text`. Measured after the fact — 0 references originate inside all sixteen tables. The
auto-analysis hazard would only have fired had they been defined as **pointer** arrays, i.e. the
wrong action.

Parsing is **two-pass** (`LoadGLS` runs `ParseGSH` twice over the same file):

1. **Pass 1** (lexer bootstrap token `0x101`): forward-declares symbols, processes the
   preprocessor. Objects get registered in the symbol table so later references resolve.
2. **Pass 2** (bootstrap token `0x102`): full parse. Objects are built as
   `ParsedThingBase` instances, fields validated and stored, and every completed object
   appended to the global `ParsedObjList`.

After parsing, the caller converts parsed objects into live game data by invoking the
`toGameObject` vtable slot on each list entry (`ConvertParsedObjects`).

## Surface Syntax

```
// single-line comment
/* nested block comments are supported (with warnings) */

#include "defaults.gsh"
#define SYMBOL
#ifdef SYMBOL / #ifndef / #else / #endif

// section [name] [: parent] { field value ... }
abstract character Chr_Default          // 'abstract' = may be incomplete; only
{                                       // usable as an inheritance parent
    walking speed 1                     // field keywords may contain spaces
    aggression    0.7
}

character Chr_Goodie : Chr_Default      // inheritance: copies all parent values
{
    strength 20
}

role Rol_PlacedObject
{
    character  none                     // 'none' clears/leaves a field empty
    shape      Shp_Barrel               // reference to a named object
    projectile ProjDef                  // or inline anonymous definition:
    light      { red 1 green 1 blue 1 range 10 }
    per vertex fogging yes              // booleans: yes/no
    ai pickup                           // enum keyword values
}

destructibility Des_Explode { type explode }
```

Observed value forms: integers, floats, quoted strings, `yes`/`no`, `none`,
enum keywords (`ai bot`, `type explode`, `action on death must`,
`resistance resists laser`), named object references and inline `{...}` definitions.
Time values can be given in seconds/minutes (grammar converts to ticks using the
current frame rate; the ` * 60.0` variant handles minutes).

## Pipeline Functions (offsets from base)

| Offset | Name | Notes |
|--------|------|-------|
| 0x00474540 | `LoadGLS(char* file, int mode)` | Entry point. Returns `ParsedObjectList*`. `mode` is a bitmask checked by each type's *filter* vtable slot; all standard types test bit 0, so pass `1`. |
| 0x00474920 | `GSHTokenize` | flex lexer; tables at 0x00652938-0x006629cc, see above |
| 0x00478400 | `ParseGSH` | Berkeley yacc parser (all grammar actions); tables at 0x006a3b68-0x006a5458, see above |
| 0x004747b0 | `ConvertParsedObjects(ParsedObjectList*)` | Calls `toGameObject` (vtbl slot 7) on every parsed object, releasing refs and updating the loading bar. |
| 0x00474870 | `FreeParsedObjectList(ParsedObjectList*)` | Releases remaining refs and frees the list itself. |
| 0x00477000 | `PrintParseError(fmt, ...)` | Increments `ParseErrorCount`, which **nothing reads** - see below |
| 0x00477050 | `PrintParseWarning(fmt, ...)` | Increments `ParseWarningCount`, likewise unread |
| 0x00477140 | `PushFileToParserStack(File*)` | Pushes an input source; `#include` support, and the seam an in-memory parse uses |
| 0x00477f70 | `PopFileFromParserStack` | |
| 0x00477ac0 | `File::ReadFile(buf, size)` | Source vtbl slot 1: `fread`, then top up from an in-memory tail |
| 0x00477aa0 | `File::GetFileName` | Source vtbl slot 2 |
| 0x004779f0 | `File::Dtor` | `fclose` if non-null, then frees `file_name` and `text_buffer` with the game's `free` |
| 0x004770a0 | `ReadRecordedText(buf, size)` | The *pass-2* source's slot 1: replays recorded chunks, no `FILE*` at all |
| 0x00477b10 | `RecordParsedText(ParseData*, char*)` | Appends one chunk to the pass-1 recording |
| 0x0047aa70 | `ClearParseSymbolTables` | |
| 0x0047b5f0 | `IsSymbolDefined(char*)` | |
| 0x0047b670 | `RegisterInSymbolTable(char*, ParsedThingBase*)` | |
| 0x0047b7d0 | `ScriptGetSymbol(char*)` | Returns `SymbolEntry*` |
| 0x0047bba0 | `FinalizeAndRegisterObject(ParsedThingBase*)` | Warns "default value assumed" for defaulted fields, then calls vtbl slot 6. |
| 0x00483fc0 | `ParsedThingBase::AddComplexField(this, name, id, sectionFn, allowNone, warnIfDefaulted)` | Registers an object-valued field |
| 0x00466e20 | `ParsedThingBase_Ctor(this, sectionFn)` | Zeroes all 137 field slots |
| 0x0047bae0 | `ParsedThingBase_Dtor` | |
| 0x0047bc00 | `IsValid` (shared vtbl slot 1) | |
| 0x0047bc30 | `IsValidDeep` (shared vtbl slot 2) | |
| 0x0047bc90 | `CopyFields` (shared vtbl slot 4) | inheritance |
| 0x0047bd60 | `CheckValue` (shared vtbl slot 5) | field assignment + validation |
| 0x0047c010 | `InitPlacedActorEntry` | placed-actor list entries (level scripts) |
| 0x004ae570 | `GetShape(char* file, char* name)` | Loads shape from a .rif. Note the order: ECX = file, EDX = name (`ToShape` passes field 0x01 then field 0x00). Same for `GetHierarchy` @ 0x004ae390. |
| 0x004add90 | `CreateRole()` | Allocates Role, assigns id, inserts in roles hash |
| 0x00579000 | `GetResourceString(&LocalizedStrings, id)` | resolves localized text ids |

### Section constructors (grammar dispatch, keyword -> function)

The grammar maps each section keyword to a constructor function. That same function
pointer is stored in `ParsedThingBase.parser_func` and is the reliable **runtime type
tag** for a parsed object.

| Section keyword | Entry fn (parser_func) | Real ctor | Parsed vtbl |
|-----------------|------------------------|-----------|-------------|
| `shape` | `ParseShape` @ 0x47c1b0 | (same) | 0x6630b0 |
| `hierarchy` | `ParseHierarchy` @ 0x47c290 | (same) | 0x6630d0 |
| `pgenerator` | `ParseParticleGenerator` @ 0x47c3c0 | (same) | 0x6630f0 |
| `light` | `ParseLight` @ 0x47e050 | (same) | 0x6631b0 |
| `projectile` | `ParseProjectile` @ 0x47e2f0 | (same) | 0x6631d0 |
| `destructibility` | `ParseDestructibility` @ 0x47e5d0 | (same) | 0x663214 |
| `frag data` | `ParseFragData` @ 0x47e6c0 | (same) | 0x663238 |
| ??? (name+replace) | `ParseUnk2` @ 0x47e9e0 | (same) | 0x66325c |
| `role` | `DoParseRole` @ 0x47cb90 | `ParseRole` @ 0x482bb0 | 0x663110 |
| `character` | `DoParseCharacter` @ 0x47db00 | `ParseCharacter` @ 0x4821b0 | 0x663190 |
| `ammo` | `DoParseAmmo` @ 0x47d6c0 | `ParseAmmo` @ 0x481d30 | 0x663130 |
| `ammo info` | `DoParseAmmoInfo` @ 0x47d870 | `ParseAmmoInfo` @ 0x481fa0 | 0x663150 |
| `camera track` | `DoParseCameraTrack` @ 0x47d970 | `ParseCameraTrack` @ 0x482100 | 0x663170 |
| `map` | `DoParseMap` @ 0x47ed90 | `ParseMap` @ 0x4829c0 | 0x663298 |
| `directory` | `DoParseDirectory` @ 0x466ba0 | `ParseDirectory` @ 0x466da0 | 0x652264 |

`DoParseXxx` merely `malloc`s the object (0x1b60 bytes; `role` allocates 0x1b68 - the
extra dword at +0x1b60 caches the converted `Role*`) and calls the real ctor.

## Parser input sources: the parse does not need a file

**The parser reads through a source object, not a `FILE*`.** Both implementations share a
3-slot vtable `{dtor, Read(buf, size), GetFileName()}`, and one of them has no file in it
at all - which is what makes `gls::ParseSource` (`src/GLS.cpp`) possible, and what killed
the last of GkPlus's generated-file tricks.

| Source | vtbl | Size | Slots | Used for |
|---|---|---|---|---|
| `File` | 0x00652904 | 0x14 | `File::Dtor` / `File::ReadFile` / `File::GetFileName` | pass 1, and every `#include` |
| `ParseData` | 0x0065292c | 0x4c | `ParseDataSource_Dtor` / `ReadRecordedText` / `ParseDataSource_GetFileName` | pass 2, replaying the recording |

`File` (built inline by `LoadGLS`, so there is no constructor to call):

| Offset | Field | Notes |
|---|---|---|
| +0x00 | vtbl | 0x00652904 |
| +0x04 | `gls_file` | `FILE*` from `_fopen(name, "r")` @ 0x005f067e. **May be null** |
| +0x08 | `text_buffer` | in-memory tail, pool memory; `File::Dtor` frees it |
| +0x0c | `text_cursor` | read cursor into it |
| +0x10 | `file_name` | pool memory; freed by the dtor |

`File::ReadFile` is already a hybrid - it reads the file and then **tops up from
`text_cursor` whenever the `fread` came up short**:

```c
n = gls_file ? fread(buffer, 1, size, gls_file) : 0;
while (n < size && *text_cursor) buffer[n++] = *text_cursor++;
```

`LoadGLS` puts a 2-byte `"\n"` there, so a file whose last line has no newline still
lexes. Point that buffer at a whole script and leave `gls_file` null and the parser reads
it entirely from memory, through the game's own class.

Three measurements are what make that a supported path rather than a stunt:

- **A null `FILE*` is not fatal.** `LoadGLS` reports it with `PrintParseError("unable to
  open file '%hs'")`, which increments `ParseErrorCount` @ 0x00739a38 - and that global
  has **no readers anywhere in the binary** (one zeroing write in `LoadGLS`, two `INC`s in
  `PrintParseError`/`PrintFatalError`, nothing else). Same for `ParseWarningCount`
  @ 0x00739a3c. So what poisons the parser after a syntax error is the un-reset file
  stack, *not* an error count, and an unopenable root file costs one invisible message.
- **`PushFileToParserStack` is the only seam.** It takes the source as its one argument,
  and it is called exactly five times in the binary: twice by `LoadGLS` (the pass-1 file,
  then the pass-2 recording) and three times by `ParseGSH` for `#include`. A detour there
  can swap `text_buffer` between construction and the first read. GkPlus's `GlsSystem`
  installs exactly that.
- **The pass-2 source proves the abstraction.** `ReadRecordedText` walks a list-of-lists
  of recorded chunks, emitting one space between chunks, and its object has no `FILE*`
  field - so `Read` + `GetFileName` really is the whole interface.

Two traps for anyone building a source:

- **`text_buffer` is freed with the game's `free`**, i.e. the pool (`File::Dtor`
  @ 0x004779f0). A replacement has to come from `pool_alloc`, never from this DLL's
  `::malloc` - see the heap discussion in `src/Memory.h` - and the buffer being replaced
  wants freeing the same way.
- **The source's name is re-lexed.** When `PreviousScriptFile` is set (i.e. during pass 1)
  `PushFileToParserStack` does `sprintf(buf, "\n# line 1 \"%hs\"\n", name)` and hands it
  to `RecordParsedText`, so pass 2 lexes that directive back. A **double quote** in the
  name therefore corrupts the parse. Backslashes are fine - every shipped script has
  `file "levels\level01.rif"` in it - which is why GkPlus's synthetic level identity is
  path-shaped (`gkplus\<slug>.gls`) and its probe names are `<gkplus probe>`-style.

`gls::ParseSource(source, display_name, mode)` is the wrapper; `gls::SourceTextScope` is
its arming half, for a caller that is itself inside a `LoadGLS` hook and must reach the
original through its own trampoline. The arming is one-shot, consumed by the first pushed
source whose name matches, so an `#include` inside the text still reaches the real file.

`src/CustomLevel.cpp` (a level's `includes` prelude), `gls::TryParse` and
`gls::ProbeKeywords` all use it, and none of the three writes a file any more.

**Verified in the running game**, not just reasoned about:

- `gls.try_parse('shape … { name "Land" file "levels\\level01.rif" }')` returns **1** - the source
  text reaches the parser and produces an object, from a name that is not a file.
- The same with `#include "scripts\defaults.gsh"` returns **6**, so an `#include` inside an
  in-memory source resolves and contributes objects like any other.
- A custom level naming six `.gsh` files had **140 roles registered** by the time its `define` hook
  ran, with nothing written to disk. The bare `#include "defaults.gsh"` spelling is the correct one
  during a load: `LoadLevel` sets the cwd with `SetCurrentDirectoryToGLDir(0)` at 0x004e0e28,
  immediately before the `LoadGLS` call, and GL dir 0 is Scripts. At the *front end* the cwd is the
  game root instead, which is why the same probe returns 1 there and 6 with a `scripts\` prefix -
  worth knowing when using `gls.try_parse` from the REPL.

## Global State (offsets from base)

| Offset | Type | Name |
|--------|------|------|
| 0x007b3cfc | `ParsedObjectList*` | `ParsedObjList` - result list built during pass 2 |
| 0x007b3d00 | int | `g_LoadGLS_Param2` - mode bitmask from `LoadGLS` |
| 0x00739a4c | int | `g_ParsePassCounter` - 1/2 before each pass, 0 while running |
| 0x007399c0 | `ParseData*` | `PreviousScriptFile` - the pass-2 source; non-null during pass 1 is also what makes `PushFileToParserStack` emit a `# line` directive |
| 0x00739a38 | int | `ParseErrorCount` - incremented by `PrintParseError`/`PrintFatalError`, **read by nothing** |
| 0x00739a3c | int | `ParseWarningCount` - same, for `PrintParseWarning` |
| 0x00739a50 | int | `g_CurrentLineNumber` |
| 0x00739a58 | char[] | `g_CurrentFilename` |
| 0x007b3cd8 | list | `g_ConditionalStack` (#ifdef nesting) |
| 0x007b3cdc | int | `g_ConditionalStackDepth` |
| 0x007b5d40 | `AmmoInfo[20]` + `Ammo*[]` | `AmmoInfos` - ammo tables (see ToAmmo) |
| 0x007b48d4 | int | `next_entity_id` - sequential Role id source |
| 0x007b48f0 | int | `num_entities` (roles hash: also 0x7b48f8 mask, 0x7b48fc buckets) |
| 0x007b6dcc | char[] | `ScriptFileName` - level .gls path used by LoadLevel |

## Data Structures

### ParsedThingBase (0x1b60 bytes)

One instance per parsed object, of any section type. Everything is indexed by a
**global field id** (0x00..0x88, 137 slots). Each section type's ctor declares which
ids it accepts, their types, names (for messages), ranges and defaults.

```
0x0000: ParsedThingVtbl* vtbl
0x0004: void*            link            // symbol-table backlink
0x0008: short            flags           // lo byte: bit0 = declared 'abstract'
                                         // hi byte: set 1 by FinalizeAndRegisterObject
0x000c: int              ref_count
0x0010: void*            parser_func     // section entry fn = type tag (see table)
0x0014: ParsedFieldType  field_types[137]     // None/Boolean/Integer/Float/Custom/String
0x0238: ParsedValue      parsed_values[137]   // 8-byte union: bool/int/double/ptr/char*
0x0680: ParsedValue      min_values[137]      // for String/Custom: .bool = "none allowed"
0x0ac8: ParsedValue      max_values[137]
0x0f10: ComplexParsedValue complex_fields[137] // per-id list of allowed sub-ctors
0x17a0: bool             field_satisfied[137]  // Ghidra name: field_mandatory (see below)
0x1829: bool             field_has_value[137]  // value present (assigned or inherited)
0x18b2: bool             is_defined[137]       // assigned in THIS object (dup detection)
0x193c: char*            field_names[137]      // keyword names, used in messages
```

`ParsedFieldType`: `None=0, Boolean=1, Integer=2, Float=3, Custom=4, String=5`.
Integer/Boolean values are stored in the low 4 bytes of the slot; Float uses the full
8 bytes (double); String is a heap copy; Custom is a `ParsedThingBase*` (ref-counted).

**Required vs defaulted fields.** `field_satisfied[i]` is initialized by the ctor and
set to `true` whenever the field is assigned:

- ctor `true` -> field is *optional with default*; if never assigned,
  `FinalizeAndRegisterObject` warns "default value assumed for '%s'".
- ctor `false` -> field is *required*; if never assigned, `IsValid` fails, the object
  is only usable as an `abstract` parent, `LoadGLS` warns "incomplete object
  definition found" and `ToXxx` converters return NULL.

### Enum keyword fields: vocabulary known, values not

Several Integer fields take a keyword rather than a number (`ai bot`, `type explode`,
`action on death must drop`). The keywords are compiled into the **flex DFA** as state
transitions, so they are *not* strings in `gl.exe` — searching the binary for `must drop`,
`resists laser`, `corona` or `plasma pistol` finds nothing. Nor are they in any shipped
file: `Scripts\glresrc.h` / `glresrc_enum.h` only carry `GL_*` resource ids (whose
numbering is unrelated — `GL_AMMO_*` runs 7600..7626, 27 entries, against an `ammo type`
space of 0..19), and `Sounds.h` is sound ids.

What the shipped scripts *do* pin is the complete vocabulary. Harvested from every
`Scripts\*.gsh` and `*.gls`:

| Field | Id | Status |
|-------|----|--------|
| `ai` | 0x49 | **recovered**, and matches `enum class AIType` exactly — the control |
| `type` (destructibility) | 0x41 | **recovered** — `explode` 0, `splatter` 1, matching `DestructibilityKind` |
| `type` (pgenerator) | 0x41 | **recovered** — confirms 7 console-table ids and adds `corona` 7, `laser trail` 10 |
| `action on death` | 0x5d | **recovered** — `must drop` 1, `must not drop` 2 |
| `resistance` | 0x4e | **recovered** — 2/4/6/8, and *not* a bitmask |
| `ammo type` | 0x17 | **recovered**, complete 0..18 |
| `weapon type` | 0x18 (ammo) | **recovered**, 31 of 34 |
| `weapon` / `secondary weapon` | 0x18/0x19 (character) | **partly** — 0..14 confirmed identical to `weapon type`; see below |

### `weapon` and `weapon type` are one enum with two bounds

They share field id 0x18, and probing a `character` returned exactly the values probing an
`ammo` did for the same keywords (0..14). What differs is the declared range:

| Section | Field | max_values[0x18] |
|---------|-------|------------------|
| `ammo` | `weapon type` | **33** |
| `character` | `weapon` / `secondary weapon` (0x19) | **INT_MAX** |

So `weapon type` is the first 34 entries - the things that are weapons with ammo, which is
all the `Ammo` table can index at `ammo_type + weapon_type * 19` - and `character.weapon`
is a wider **inventory item** space. The shipped pickups confirm it: `body_slot_upgrades.gsh`
has real `character` sections with `weapon audio cloak`, `weapon lock decoder`,
`weapon terrain scanner`, and `ammo_pickups.gsh` has `weapon autolock bolts`. None of those
can fit under 34, so they occupy ids above 33 - not yet recovered, because probing stopped
before reaching them.

**`weapon enemy laser` looks like a shipped bug.** The probe refuses it, and the only use is
`guardian_turret.gsh`, reached from the multiplayer level `mplay_tf_oilrig01.gls`. That
same character also sets `sight angle 90`, which is outside the field's declared 0..89 - so
the section almost certainly logs errors at load and nobody noticed.

### Declared bounds, for the enum fields

Straight off the section constructors, and useful for knowing when a recovered table is
complete:

| Section | Field | Range | Recovered |
|---------|-------|-------|-----------|
| `destructibility` | `type` 0x41 | 0..1 | **complete** |
| `pgenerator` | `type` 0x41 | 0..12 | 12 of 13 - only id **8** unnamed |
| `ammo` | `ammo type` 0x17 | 0..19 | 0..18, so 19 is unused |
| `ammo` | `weapon type` 0x18 | 0..33 | 31; 10, 15 unnamed and 33 = the keyword-less default |
| `role` | `resistance` 0x4e | 0..9 | 4 of them, at 2/4/6/8 |
| `role` | `action on death` 0x5d | 0..INT_MAX | 2, plus 0 = unspecified |
| `character` | `weapon` 0x18 | 0..INT_MAX | 0..14 so far |

### Recovered by probing (`gls.probe`)

`ammo type` (0x17), complete and a clean bijection onto 0..18 — no gaps or duplicates,
which is the check that it is right:

| | | | | |
|---|---|---|---|---|
| 0 needles | 1 flares | 2 plasma bolts | 3 plasmaxi bolts | 4 plasma shells |
| 5 autolock bolts | 6 battery basic | 7 battery plus | 8 energy cells | 9 grenade basic |
| 10 grenade plus | 11 grenade EMP | 12 missile EMP | 13 missile basic | 14 missile plus |
| 15 flames | 16 napalm | 17 nanotech dismantler | 18 none needed | |

`weapon type` (0x18), 31 of the 34-value space — every keyword any shipped script uses.
Player weapons occupy the low ids, enemy weapons the high ones:

| | | | |
|---|---|---|---|
| 0 plasma pistol | 1 plasma pistol training | 2 plasmagnum | 3 plasmatrix |
| 4 laser | 5 binary laser | 6 maxim laser | 7 grenade launcher |
| 8 missile launcher | 9 flamethrower | 11 nanofrag | 12 epulsar |
| 13 repair arm | 14 interface arm | 16 enemy plasma weak | 17 enemy plasma medium |
| 18 enemy plasma strong | 19 enemy laser weak | 20 enemy laser medium | 21 enemy laser strong |
| 22 enemy grenade launcher basic | 23 enemy grenade launcher plus | 24 enemy missile launcher basic | 25 enemy missile launcher plus |
| 26 enemy missile launcher basic slow reload | 27 enemy laser adversor | 28 enemy plasma pulsax | 29 enemy plasma pulsox |
| 30 enemy plasma mini pulsox | 31 enemy epulsar obliteron | 32 enemy epulsar obliteron deadly | |

Missing: **10** and **15**, which no shipped script names, and **33**, the ctor default
that means "none" (see below - it has no spelling).

The four small ones, all confirmed against existing enums where one existed:

| Field | Values |
|-------|--------|
| `type` (destructibility) | 0 `explode`, 1 `splatter` — matches `DestructibilityKind` |
| `action on death` | 1 `must drop`, 2 `must not drop`; 0 is the default and has no keyword |
| `resistance` | 2 `resists laser`, 4 `resists explosives`, 6 `resists epulsar`, 8 `resists small arms` |
| `type` (pgenerator) | 0 `smoke`, 1 `steam`, 3 `fire`, 4 `shot`, 5 `explosion`, 6 `big explosion`, **7 `corona`**, **10 `laser trail`**, 12 `sparks` |

Two of those deserve a note:

- **`resistance` is not a bitmask.** The values step by 2 (2/4/6/8), not by powers of two,
  so 1/3/5/7 are unaccounted for and the four keywords cannot be combined. An earlier guess
  in this file that "four values reads like a bitmask" was wrong.
- **`corona` = 7 and `laser trail` = 10** fill two of the three ids
  `GetParticleIDFromName` leaves unnamed; **8 is still unknown**. The probe also
  independently reproduced smoke/steam/fire/shot/explosion/big explosion/sparks at the ids
  the console table gives, which is a second check on `ParticleType` in `src/Roles.h`.

**The `ai` row is the control and it passed**: all 21 keywords came back at exactly the
values `enum class AIType` declares, in order, including the non-obvious middle
(`reserved` 4, `waiting` 6, `pathfinder` 7, `swarm` 17). Together with `destructibility`
matching `DestructibilityKind`, that is two independent confirmations that the method
reports what the game actually stores.

### Five traps the probing exposed

The first is the one that matters to anything calling `LoadGLS`, not just to probing.

- **A syntax error poisons the parser for the rest of the process.** `LoadGLS` resets its
  error counter, `ParsedObjList` and the symbol tables on entry, but evidently not the file
  stack, and *nothing recovers* - after one bad parse, a verbatim copy of a shipped section
  fails identically. This was established by bisection: six `destructibility` variants all
  parsed when run first, and the same text returned -1 once a syntax error had happened
  earlier in the run. Any tool making repeated `LoadGLS` calls has to treat the first
  failure as terminal.
- **An unrecognised keyword is a `syntax error`, not a per-value rejection**, so it takes
  the rest of the file with it *and* poisons everything after. `gls::ProbeKeywords` parses
  one keyword per call and stops at the first refusal for both reasons.
- **`none` is not spellable on an integer enum field.** It is a value form for String and
  Custom fields only; `weapon type none` is a syntax error even though 33 (= none) is that
  field's own default. The ctor default is reachable only by omitting the field.
- **An object missing a required field is silently demoted to abstract and left OUT of
  `ParsedObjList`.** The warning is "abstract definition not declared with 'abstract'",
  and `LoadGLS` then reports "empty script found" for a file where every section did in
  fact parse. Anything reading the returned list has to make its objects complete first.
  The trap inside the trap: **`pgenerator`'s required `life` is invisible to reflection**,
  because field id 0x42 is intercepted by that section's `CheckValue` override and stored
  in the 0x1b70 object's extension, so `field_types[0x42]` is never set. A pgenerator
  built from `SectionFields` alone is always incomplete.
- **`field_names[]` *is* the lexer spelling** - a hypothesis to the contrary was tested and
  refuted: `nolighting` parses and `no lighting` is a syntax error, likewise
  `generategenerators`. The array is safe to generate script text from.

Two things worth knowing before trusting that table further:

- **`corona` and `laser trail` are particle types the console keyword table does not
  know.** `GetParticleIDFromName` @ 0x0044c340 is where `ParticleType` (`src/Roles.h`)
  came from, and it leaves 7, 8 and 10 unnamed. The GLS lexer accepts these two, so they
  are two of those three ids — which one is which has not been established.
- **The `weapon` keyword harvest is not clean.** 64 distinct values turn up under `weapon
  <x>`, well past the 34-value space the constructor allows, and they include ammo names,
  gadgets (`audio cloak`, `lock decoder`, `terrain scanner`) and two object references
  (`Wpn_BlueLaser`). So `weapon` is being used for more than one field, or for an
  inventory-item space wider than `character`'s. Do not treat that list as the character
  `weapon` enum without separating the sections first.

Recovering the actual numbers needs either the flex DFA simulated out of `GSHTokenize`
@ 0x00474920 (feed each keyword through the transition tables for its rule number, then
read the `yylval` constant that rule's action loads) or a running game to observe the
converted values. Neither has been done.

### The schema is in the object: reflection

Everything the tables further down this file record by hand is also **present at runtime**,
written into each instance by its own section constructor. For any parsed object:

| Array | What it tells you |
|-------|-------------------|
| `field_types[id]` | `None` means this section does not accept the id at all |
| `field_names[id]` | the GLS keyword, e.g. `"walking speed"` |
| `field_satisfied[id]` | as *initialized*: `true` = has a default, `false` = required |
| `min_values[id]` / `max_values[id]` | the bounds `CheckValue` enforces |
| `min_values[id].boolean` | String and Custom only: "`none` is allowed" |

So constructing one throwaway instance per section type recovers the whole schema, and no
hand-written table can drift from the binary. `gk::gls::SectionFields` (`src/GLS.cpp`) does
exactly that and is what the `gls` JS bindings build their field names from. The per-section
tables below stay useful as a reference, but the object is the authority.

### Two hazards when reusing parsed objects

Both matter as soon as parsed objects are built programmatically and kept, which is what
`src/JsGls.cpp` does.

- **`ToRole` caches its result at `parsed+0x1b60`** (the extra dword a role's 0x1b68
  allocation carries) and is gated on that being null. `DestroyRoles` frees every `Role`
  between levels, so converting the same parsed role on a second level load hands back a
  pointer into a freed pool page. `gk::gls::ResetConversionCache` clears the dword; the
  other section types have no cache and are unaffected.
- **`CheckValue` applies no unit conversion** - it range-checks and stores (0x0047bd60 is a
  duplicate check, a type switch, a bounds compare and a store, nothing else). The stored
  value is in `.gls` units, which the declared defaults confirm: `scan delay` defaults to
  0.5 s and `sight angle` is bounded 0..89 degrees. Seconds become ticks and degrees become
  radians inside `ToCharacter`, i.e. *after* the parsed object. A value set programmatically
  therefore means exactly what the same literal means in a `.gsh`.

### Recovering a parsed object's symbol name

`ParsedThingBase.link` @ +0x04 is the symbol-table backlink, and the entry it points at is the
8-byte `{ParsedThingBase *thing, char *name}` that `SymbolEntry_Ctor` builds (see
`RegisterInSymbolTable` @ 0x0047b670, which hashes `entry[1]` as the string). So the `Rol_Bug`
in `role Rol_Bug { ... }` is reachable from the parsed object itself - which is what a
`.gsh` -> `.mjs` transpiler would need, since `ClearParseSymbolTables` @ 0x0047aa70 wipes the
table before `LoadGLS` returns. Not yet used by anything.

### ParsedThingVtbl (8-9 slots)

| # | Name | Shared impl | Purpose |
|---|------|-------------|---------|
| 0 | dtor | per-type | scalar deleting dtor |
| 1 | isValid | 0x47bc00 | all declared fields satisfied |
| 2 | isValidDeep | 0x47bc30 | isValid + recursively isValid on Custom sub-objects |
| 3 | filter | per-type (all `mode & 1`) | should this type be processed for LoadGLS mode |
| 4 | copyFields | 0x47bc90 | inheritance: copy values/satisfied/has_value from parent, strdup Strings, ref++ Customs |
| 5 | checkValue | 0x47bd60 | assign one field (see below) |
| 6 | registerObject | 0x47bff0 (nop) | called by FinalizeAndRegisterObject |
| 7 | toGameObject | per-type | convert to live game data (nop for sub-object types) |
| 8 | *(destructibility types only)* | per-type | `ToDestructibility` / `ToFragData` / `ToReplaceDestructibility` |

### CheckValue semantics (vtbl slot 5, 0x47bd60)

Given `(field_id, value)`:
1. `is_defined[id]` already set -> error "value for '%s' is defined more than once".
2. Type mismatch -> "invalid data type"; numeric bounds -> "cannot be less/more than".
3. String/Custom `none` (null) with `min_values[id].bool == false` -> error
   "value for '%s' cannot be none".
4. Custom: value's `parser_func` must appear in `complex_fields[id]`'s allowed-ctor
   list, else "value for '%s' is not a suitable type". Stores pointer, `ref_count++`.
5. Sets `field_satisfied`, `field_has_value`, `is_defined`.

### ParsedObjectList (0x10) / list nodes

Same sentinel-based doubly-linked layout as TriggerList (see trigger_system_notes.md):
`{sentinel, count, cached_array, flag}`; nodes are `{vtbl, prev, next, ParsedThingBase* thing}`
(payload at +0xC). Placed-actor entries created by `InitPlacedActorEntry`
(`{vtbl=ParsedThingBaseVtbl@0x652244, 9, SymbolEntry* role, pos, orient}`) also travel
through this list.

## Field ID Master Table

Global ids consolidated from all 15 ctors (name column = keyword; a blank cell means
the id is unused by that section). Types: B=Boolean, I=Integer, F=Float, S=String,
C=Custom(sub-object).

| ID | Keyword | Used by |
|----|---------|---------|
| 0x00 | name | shape, hierarchy, ???, ammo, camera track, map, directory |
| 0x01 | file (labelled "directory" in `directory`) | shape, hierarchy, ammo, camera track, map, directory |
| 0x02 | bitmap | map |
| 0x03 | hotspot | hierarchy |
| 0x04 | alternate hotspot | hierarchy |
| 0x05 | shape | role, ammo info |
| 0x06 | inventory shape | role |
| 0x07 | pgen | role, camera track |
| 0x08 | pgen2 | role, camera track |
| 0x0a | character | role |
| 0x0b | projectile | role (ProjParsed), ammo (Role!) |
| 0x0c | walking speed | character |
| 0x0d | turning speed | character |
| 0x0e | scan delay | character |
| 0x0f | scan acceptance angle | character |
| 0x10 | angular scan rate | character |
| 0x11 | mine laying time | character |
| 0x12 | magazine size | ammo |
| 0x13 | salvo size | ammo |
| 0x14 | round time | ammo |
| 0x15 | reload time | ammo |
| 0x16 | life timer | ammo |
| 0x17 | ammo type | ammo, ammo info |
| 0x18 | weapon type | ammo |
| 0x19 | secondary weapon | character |
| 0x1a | description | role, character, ammo info |
| 0x1b | pickup name | role |
| 0x1c | ammo name | ammo info |
| 0x1d | max per slot | ammo info |
| 0x1e | light | role |
| 0x1f | hit_light | projectile |
| 0x20 | sound | projectile, ammo |
| 0x21-0x23 | red, green, blue | light, pgenerator |
| 0x24 | alpha | role, pgenerator |
| 0x25-0x27 | specular red/green/blue | light |
| 0x28 | range / blast range | light(range), projectile+frag data(blast range) |
| 0x29 | strength | character |
| 0x2a | damage | projectile |
| 0x2b | damage multiplier | character |
| 0x2c | blast damage | projectile, frag data |
| 0x2d | max range | projectile |
| 0x2e | shot speed multiplier / firing speed | character, ammo |
| 0x2f | target cycle time | character |
| 0x30 | weapon cycle time | character |
| 0x31 | weapon cycle time2 | character |
| 0x32 | alarm delay | character |
| 0x33 | gravity | projectile |
| 0x34 | aim | character |
| 0x35 | sight angle | character |
| 0x36 | gun yaw angle | character |
| 0x37 | elevation angle | character |
| 0x38 | sight range | character |
| 0x39 | hearing range | character |
| 0x3a | alert radius | character |
| 0x3b | aggression | character |
| 0x3c | can turn | character |
| 0x3d | draw vision cone | character |
| 0x3e | draw hearing range | character |
| 0x3f | status window u | character |
| 0x40 | status window v | character |
| 0x41 | type | pgenerator, destructibility |
| 0x43 | rate | pgenerator |
| 0x44-0x46 | x, y, z | pgenerator |
| 0x47 | identifier | role |
| 0x48 | sever point | role |
| 0x49 | ai | role |
| 0x4a | interface beam delay | role |
| 0x4b | interface beam effect | role |
| 0x4c | interface beam script | role |
| 0x4d | interface beam duration | role |
| 0x4e | resistance | role |
| 0x4f | resistance factor | role |
| 0x50 | camera plane | map |
| 0x51 | max camera distance | map |
| 0x52 | max camera focus height | map |
| 0x53 | min camera focus height | map |
| 0x54 | shadow object rif | map |
| 0x55 | shadow object name | map |
| 0x56 | alpha fogging | role |
| 0x57 | reflective | role |
| 0x58 | per vertex fogging | role |
| 0x59 | destructibility | role |
| 0x5a | blob shadow | character |
| 0x5b | armour | role |
| 0x5c | shields | role |
| 0x5d | action on death | role |
| 0x5e | pickup radius | role |
| 0x5f | recharge rate | role |
| 0x60 | role | frag data |
| 0x61 | replace role | frag data |
| 0x62 | remove | frag data |
| 0x63 | scale | frag data |
| 0x64 | start scale | pgenerator |
| 0x65 | end scale | pgenerator |
| 0x66 | spin | pgenerator |
| 0x67 | particle TTL | pgenerator |
| 0x68 | generategenerators | pgenerator |
| 0x69 | replace | frag data, ??? |
| 0x6a | symmetric | frag data |
| 0x6b | radius | character |
| 0x6c | height | character |
| 0x6d | size | character |
| 0x6e | destination selectable | role |
| 0x6f | destroy after collection | role |
| 0x70 | moves on lifts | role |
| 0x71 | status display | role |
| 0x72 | hit test ignore | role |
| 0x73 | nolighting | role |
| 0x74 | always cpu controlled | character |
| 0x75 | frag control | role |
| 0x76 | customisation hierarchy | character |
| 0x77 | shadow hierarchy | character |
| 0x78 | limit | role |
| 0x79 | switch size | role |
| 0x7a | max vertices per section | map |
| 0x7b | generation limit | character |
| 0x7c | alertable | character |
| 0x7d | latch trigger | character |
| 0x7e | recon name | role |
| 0x7f | recon ai short | role |
| 0x80 | recon ai number | role |
| 0x81 | recon ai long | role |
| 0x82 | recon ai long2 | role |
| 0x83 | max weapon | character |
| 0x84 | max ammo | character |
| 0x85 | max module | character |
| 0x86 | initial first person range | character |
| 0x87 | maximum first person range | character |
| 0x88 | meta sound | role |
| 0x18 (dup) | weapon | character (label "weapon") |

## Per-Section Field Tables

Legend: `req` = required (object incomplete without it), `dflt` = defaulted (warns
"default value assumed" if omitted), `none ok` = `none` accepted, `sub=` allowed
sub-object ctors, `ref` = named references allowed. `max=DBL` means DBL_MAX
(unbounded).

### shape (`ParseShape`)
| Field | ID | Type | Rules |
|-------|----|------|-------|
| name | 0x00 | S | req, none not allowed |
| file | 0x01 | S | req, none not allowed |

### hierarchy (`ParseHierarchy`)
| Field | ID | Type | Rules |
|-------|----|------|-------|
| name | 0x00 | S | req |
| file | 0x01 | S | req |
| hotspot | 0x03 | S | dflt none, none ok |
| alternate hotspot | 0x04 | S | dflt none, none ok |

### pgenerator (`ParseParticleGenerator`)
| Field | ID | Type | Rules |
|-------|----|------|-------|
| red/green/blue | 0x21-23 | F | req, 0..DBL, def 0 |
| alpha | 0x24 | F | req, 0..1, def 0 |
| type | 0x41 | I | req, 0..12, def 0 |
| rate | 0x43 | F | req, 0..DBL, def 0 |
| x/y/z | 0x44-46 | F | req, def 0 |
| start scale | 0x64 | F | dflt 1.0 |
| end scale | 0x65 | F | dflt 1.0 |
| spin | 0x66 | F | dflt 0 |
| particle TTL | 0x67 | F | dflt 0 |
| generategenerators | 0x68 | B | dflt no |

`type` keywords seen in game scripts: shot, fire, smoke, explosion, sparks, steam,
corona, laser, big, splatter, explode (0..12 mapping via lexer keywords).

### light (`ParseLight`)
All req, 0..DBL, def 0: red 0x21, green 0x22, blue 0x23, specular red 0x25,
specular green 0x26, specular blue 0x27, range 0x28.

### projectile (`ParseProjectile`)
| Field | ID | Type | Rules |
|-------|----|------|-------|
| hit_light | 0x1f | C | dflt, none ok, sub=[light] |
| sound | 0x20 | I | dflt 104, 0..256 |
| blast range | 0x28 | F | dflt 0 |
| damage | 0x2a | F | req, def 0, unbounded (negative heals) |
| blast damage | 0x2c | F | dflt 0 |
| max range | 0x2d | F | dflt 196.0 |
| gravity | 0x33 | B | req, def no |

### destructibility (`ParseDestructibility`)
| Field | ID | Type | Rules |
|-------|----|------|-------|
| type | 0x41 | I | req, 0..1 (`explode`=?, `splatter`=?) |

### frag data (`ParseFragData`)
| Field | ID | Type | Rules |
|-------|----|------|-------|
| blast range | 0x28 | F | dflt 0 |
| blast damage | 0x2c | F | dflt 0 |
| role | 0x60 | C | req, no none, sub=[role] |
| replace role | 0x61 | C | dflt, none ok, sub=[role] |
| remove | 0x62 | S | dflt none, none ok |
| scale | 0x63 | I | req, 1.. |
| replace | 0x69 | B | dflt yes |
| symmetric | 0x6a | B | dflt no |

### ??? "replace destructibility" (`ParseUnk2`)
| Field | ID | Type | Rules |
|-------|----|------|-------|
| name | 0x00 | S | req — **a .gcs script name**, see below |
| replace | 0x69 | B | dflt no |
Converts to a destructibility record of type 4 `{vtbl, 4, char* script, bool replace}`.

**Field 0x00 is a script file name, not an identifier**, so this section means "run a script
when the object dies". The GLS keyword is `name` and that is what misled the earlier note; the
evidence is that its *only* reader is `Frag` @ 0x0052e220, whose `case 4` does
`QueueScriptExecution(destructibility + 8)` and then reads `replace` at `+0x0c` into the same
local the `frag data` path fills from `FragData::replace`. `ToReplaceDestructibility`
@ 0x0047eaa0 strdups it and `ReplaceDestructibilityDtor` @ 0x00483d00 frees it; nothing else
touches the field. It is therefore modelled as `ReplaceDestructibility::script` in `src/Roles.h`
and spelled `script` in `make.role`'s description. Like every script-name field it carries JSON
under GkPlus: `ToReplaceDestructibility` is hooked so the name it strdups is stored as the JSON
string `"x.gcs"`, and a `make.role` description may put a message object there instead
(`src/ScriptQueue.h`). The same hook exists on `ToRole` for GLS field 0x4c
(`interface beam script`), which is the other converter that produces a script name.

The **section keyword is still unknown** — `ParseUnk2` is reached from the `destructibility`
sub-section list, and no shipped header uses it — so "replace destructibility" remains a
descriptive name, not a recovered one.

### role (`DoParseRole` / `ParseRole`)
| Field | ID | Type | Rules |
|-------|----|------|-------|
| shape | 0x05 | C | dflt, none ok, sub=[shape, hierarchy, pgenerator] |
| inventory shape | 0x06 | C | dflt, none ok, sub=[shape, hierarchy] |
| pgen | 0x07 | C | dflt, none ok, sub=[pgenerator] |
| pgen2 | 0x08 | C | dflt, none ok, sub=[pgenerator] |
| character | 0x0a | C | **req**, none ok, sub=[character] |
| projectile | 0x0b | C | **req**, none ok, sub=[projectile] |
| description | 0x1a | I | dflt 0 (localized-string id) |
| pickup name | 0x1b | I | dflt 0 (localized-string id) |
| light | 0x1e | C | **req**, none ok, sub=[light] |
| alpha | 0x24 | F | dflt 1.0, 0..1 |
| identifier | 0x47 | S | **req**, none ok (in-game name for GetRoleByName) |
| sever point | 0x48 | S | dflt none; comma-separated list |
| ai | 0x49 | I | dflt 4 (Reserved); see AI enum |
| interface beam delay | 0x4a | I | dflt -1 |
| interface beam effect | 0x4b | I | dflt 1; 4 = script (requires 0x4c) |
| interface beam script | 0x4c | S | dflt none |
| interface beam duration | 0x4d | I | dflt -1 |
| resistance | 0x4e | I | dflt 0, 0..9 (`resists <weapon class>` keywords) |
| resistance factor | 0x4f | F | dflt 0, 0..1 |
| alpha fogging | 0x56 | B | **req**, def no |
| reflective | 0x57 | B | dflt no |
| per vertex fogging | 0x58 | B | **req**, def no |
| destructibility | 0x59 | C | **req**, none ok, sub=[destructibility, frag data, ???] |
| armour | 0x5b | F | dflt 0 |
| shields | 0x5c | F | dflt 0 |
| action on death | 0x5d | I | dflt 0 (`must`, `binary` keywords) |
| pickup radius | 0x5e | F | dflt 6.0, 0..30 |
| recharge rate | 0x5f | F | dflt 0 |
| destination selectable | 0x6e | B | dflt no |
| destroy after collection | 0x6f | B | dflt yes |
| moves on lifts | 0x70 | B | dflt no |
| status display | 0x71 | B | dflt yes |
| hit test ignore | 0x72 | B | dflt no |
| nolighting | 0x73 | B | dflt no |
| frag control | 0x75 | B | dflt no |
| limit | 0x78 | I | dflt 0 |
| switch size | 0x79 | I | dflt 100, 1.. |
| recon name | 0x7e | I | dflt 0 (localized-string id) |
| recon ai short | 0x7f | I | dflt 0 |
| recon ai number | 0x80 | I | dflt -1 |
| recon ai long | 0x81 | I | dflt 0 |
| recon ai long2 | 0x82 | I | dflt 0 |
| meta sound | 0x88 | S | dflt none |

### character (`DoParseCharacter` / `ParseCharacter`)
| Field | ID | Type | Rules |
|-------|----|------|-------|
| walking speed | 0x0c | F | **req**, def 0 (cycles/sec) |
| turning speed | 0x0d | F | **req**, def 0 (revolutions/sec) |
| scan delay | 0x0e | F | dflt 0.5 s |
| scan acceptance angle | 0x0f | F | dflt 20, 0..360 deg |
| angular scan rate | 0x10 | F | dflt 50 |
| mine laying time | 0x11 | F | dflt 8 s |
| weapon | 0x18 | I | dflt 33 (=none) |
| secondary weapon | 0x19 | I | dflt 33 |
| description | 0x1a | I | dflt 0 (localized-string id) |
| strength | 0x29 | F | **req**, def 0 (hit points; also encodes pickup type: aggression*10 for pickup roles) |
| damage multiplier | 0x2b | F | dflt 1.0 |
| shot speed multiplier | 0x2e | F | dflt 1.0 |
| target cycle time | 0x2f | F | dflt 3.0 |
| weapon cycle time | 0x30 | F | dflt 0 |
| weapon cycle time2 | 0x31 | F | dflt 0 |
| alarm delay | 0x32 | F | dflt 0 |
| aim | 0x34 | F | **req**, def 0 |
| sight angle | 0x35 | F | **req**, def 0, 0..89 deg |
| gun yaw angle | 0x36 | F | dflt 60, 0..180 deg |
| elevation angle | 0x37 | F | dflt 80, 0..180 deg |
| sight range | 0x38 | F | **req**, def 0 (metres) |
| hearing range | 0x39 | F | **req**, def 0 |
| alert radius | 0x3a | F | dflt 10 |
| aggression | 0x3b | F | **req**, def 0, 0..1.01 |
| can turn | 0x3c | B | dflt yes |
| draw vision cone | 0x3d | B | dflt yes |
| draw hearing range | 0x3e | B | dflt yes |
| status window u | 0x3f | I | dflt 0, 0..1024 |
| status window v | 0x40 | I | dflt 0, 0..1024 |
| blob shadow | 0x5a | I | dflt 0, 0..1 — but the shipped headers write **`spider`** for 1 (see below) |
| radius | 0x6b | F | dflt 0 |
| height | 0x6c | F | dflt 0 |
| size | 0x6d | F | dflt 1.0 |
| always cpu controlled | 0x74 | B | dflt no |
| customisation hierarchy | 0x76 | C | dflt, none ok, sub=[hierarchy] |
| shadow hierarchy | 0x77 | C | dflt, none ok, sub=[hierarchy] |
| generation limit | 0x7b | I | dflt 5, 1..10 |
| alertable | 0x7c | B | dflt yes |
| latch trigger | 0x7d | B | dflt no |
| max weapon | 0x83 | I | dflt 0 |
| max ammo | 0x84 | I | dflt 0 |
| max module | 0x85 | I | dflt 0 |
| initial first person range | 0x86 | F | dflt 5, 5..100 |
| maximum first person range | 0x87 | F | dflt 5, 5..100 |

**`blob shadow` (0x5a) — a type question this table may be answering wrongly.** The row says `I`
(integer, 0..1), and the *consumer* agrees it is an integer: `Unit_BuildShadowNode` @ 0x005525b0 reads
`Character+0x8c` as an `int` and does `SUB EAX,0 / JZ` then `SUB EAX,1 / JNZ` (see
`role_subobjects_notes.md`). But **the shipped headers write it as a bare keyword**, not a number —
`blob shadow		spider` appears 10 times across `archore.gsh`, `mplay_archore.gsh`,
`pres_arrow.gsh` and `walking_mine.gsh`, and no shipped header writes `blob shadow 1`.

So either this field's parse rule is not plain `I`, or the parser maps that keyword to 1 somewhere
this table does not record. **NOT SETTLED — the field's entry in the `character` parse table was not
re-read for this.** Do not "fix" the type on the strength of the `.gsh` evidence alone; a `.gsh`
author sees a keyword, which is the practical fact worth knowing, and the resolution is one read of
the parse-table entry away. Note also that value 1's effect is inert unless `ShadowQuality == 1`.

### ammo (`DoParseAmmo` / `ParseAmmo`)
| Field | ID | Type | Rules |
|-------|----|------|-------|
| name | 0x00 | S | req |
| file | 0x01 | S | req |
| projectile | 0x0b | C | dflt, none ok, sub=[**role**] (projectile roles!) |
| magazine size | 0x12 | I | req, def 0 |
| salvo size | 0x13 | I | dflt 1 |
| round time | 0x14 | F | req, def 0 |
| reload time | 0x15 | F | req, def 0 |
| life timer | 0x16 | I | req, def 0 |
| ammo type | 0x17 | I | req, 0..19 |
| weapon type | 0x18 | I | req, 0..33 |
| sound | 0x20 | I | dflt 3, 0..256 |
| firing speed | 0x2e | F | req, def 0 |

### ammo info (`DoParseAmmoInfo` / `ParseAmmoInfo`)
| Field | ID | Type | Rules |
|-------|----|------|-------|
| shape | 0x05 | C | dflt, none ok, sub=[shape, hierarchy] |
| ammo type | 0x17 | I | req, 0..19 |
| description | 0x1a | I | req (localized-string id) |
| ammo name | 0x1c | I | req (localized-string id) |
| max per slot | 0x1d | I | req |

### camera track (`DoParseCameraTrack` / `ParseCameraTrack`)
| Field | ID | Type | Rules |
|-------|----|------|-------|
| name | 0x00 | S | req |
| file | 0x01 | S | req |
| pgen | 0x07 | C | dflt, none ok, sub=[pgenerator] |
| pgen2 | 0x08 | C | dflt, none ok, sub=[pgenerator] |

### map (`DoParseMap` / `ParseMap`)
| Field | ID | Type | Rules |
|-------|----|------|-------|
| name | 0x00 | S | req |
| file | 0x01 | S | req |
| bitmap | 0x02 | S | req, none ok |
| camera plane | 0x50 | S | req, none ok |
| max camera distance | 0x51 | F | req, 10..500 |
| max camera focus height | 0x52 | S | dflt none |
| min camera focus height | 0x53 | S | dflt none |
| shadow object rif | 0x54 | S | dflt none |
| shadow object name | 0x55 | S | dflt none |
| max vertices per section | 0x7a | I | dflt 200, 10..10000 |

(ParsedMap overrides isValidDeep/copyFields/checkValue - 0x52/0x53 hold strings that
are parsed to floats during ToMap.)

### directory (`DoParseDirectory` / `ParseDirectory`)
`name` 0x00 + `file` 0x01 (labelled "directory"), both req. `toGameObject` =
`ParseGLDirs` @ 0x466c20: matches name against scripts/fmvs/rifs/graphics/fonts/
dumps/sounds (or sounds22 on newer DirectX) and sets the corresponding game
directory. Loaded at startup from `gldirs.gls` via `LoadGLDirs` @ 0x466b30.

## Conversion To Game Data (`toGameObject` and helpers)

`ConvertParsedObjects` (0x4747b0) walks `ParsedObjList` calling vtbl slot 7:

| Converter | Address | Produces |
|-----------|---------|----------|
| `ToRole` | 0x47cc20 (thunk 0x47cc10) | `Role` (0xc0) via `CreateRole`; inserts into roles hash; caches result at parsed+0x1b60. Nested: ToShape/ToHierarchy/ToParticleGenerator/ToLight/ToProjectile/ToCharacter/destructibility slot 8. |
| `ToCharacter` | 0x47db80 | `Character` (0xb8), heap |
| `ToProjectile` | 0x47e4e0 | `Projectile` (0x20), heap |
| `ToLight` | 0x47e220 | `Light` (0x1c), heap |
| `ToShape` | 0x47c260 | `GetShape(file, name)` - loads from .rif (ECX = file/field 0x01, EDX = name/field 0x00) |
| `ToHierarchy` | 0x47c390 | hierarchy object from .rif |
| `ToParticleGenerator` | 0x47c750 | `ParticleGenerator` (0xd4) |
| `ToDestructibility` | 0x47e680 | `{vtbl, int type}` (8 bytes) |
| `ToFragData` | 0x47e890 | 0x24-byte record `{vtbl, 3, Role* role, Role* replace_role, char* remove, int scale, bool replace, bool symmetric, float blast_range, float blast_damage}` |
| `ToReplaceDestructibility` | 0x47eaa0 | 0x10-byte record `{vtbl, 4, char* name, bool replace}` |
| `ToAmmo` | 0x47d740 | fills `AmmoInfos` ammo slot `[weapon_type*19 + ammo_type]` (table of `Ammo*` at AmmoInfos+0x180) |
| `ToAmmoInfo` | 0x47d8f0 | fills `AmmoInfos[ammo_type]` (0x14-byte `AmmoInfo` records at +0) |
| `ToCameraTrack` | 0x47d9f0 | camera track object (0xa0) + pgen/pgen2. `AcquireLevelRifForLocators(file)` (ECX, field 0x01), `CameraTrack_Ctor` @ 0x4dc660 on `pool_alloc(0xa0)`, pgen/pgen2 -> +0x68/+0x6c, then `LoadCameraTrackFromRif` @ 0x5aa920 — `bool __fastcall(track /*ECX*/, rif /*EDX*/, name, Vec3 map origin **by value**)`, `RET 0x10`. That walks the rif's REBENVDT -> SPECLOBJ, `_stricmp`s `name` against each `CUTSHEAD` child's `CUTSCDAT` name (+0x28) and hands the match to `CameraTrack_LoadFromCutscene` @ 0x5bf060 with `*(float *)rif` as the unit scale. False destroys the track through slot 0. DEFECT: a failed `pool_alloc` is written through |
| `ToMap` | 0x47f160 | loads the level geometry itself (huge) - see `level_loading_notes.md` |
| `ParseGLDirs` | 0x466c20 | sets game resource directories |

Unit conversions applied during ToCharacter:
- angles: `stored = value` (degrees as double) -> `angle_units = deg / 360 * 4096`
  (the game uses a 4096-step circle; sin/cos tables are 0x1000 entries)
- `walking speed`: multiplied by 65536 (16.16 fixed point), then normalized by size
- `status window u/v`: multiplied by 1/1024 (texture UV)
- `sight/hearing range` are squared and cached

Role flags byte 0 (Role+0x78): bit0=alpha fogging, bit1=per vertex fogging (only if
bit0 clear), bit2=nolighting, bit3=reflective, bit4=destination selectable,
bit5=destroy after collection, bit6=hit test ignore, bit7=frag control.
Byte 1: bit0=moves on lifts, bit1=status display.
`sever point` string is split on ',' into the list at Role+0xac (count +0xb0).
`description`/`pickup name`/`recon *` ints are resolved through
`GetResourceString(&LocalizedStrings @ 0x725664, id)`.

## Enum Keyword Vocabulary (values observed in shipped scripts)

- `ai`: bot=0, scavenger=1, mine=2, minebot=3, reserved=4, blocker=5, waiting=6,
  pathfinder=7, `track object`=8, tumbleweed=9, pickup=10, `background creature`=11,
  `flying background creature`=12, centipede=13, centibody=14, node=15,
  `node waiting`=16, swarm=17, popup=18, president=19, turret=20 (AIType enum;
  determines which Actor subclass is spawned - see actor_vtable_notes.md)
- destructibility `type`: `explode`, `splatter` (0/1)
- pgenerator `type`: shot, fire, smoke, explosion, sparks, steam, corona, laser, big,
  splatter, explode... (ints 0..12)
- `action on death`: `must`, `binary`
- `resistance`: `resists small arms`, `resists laser`, `resists explosives`,
  `resists epulsar` (ints 0..9)
- booleans: `yes` / `no`; empty value: `none`
- weapon ids: ints 0..33, 33 = none (see `weapon`/`max weapon`)

## Level Load Flow (LoadLevel @ 0x4e0980)

```
SetCurrentDirectoryToGLDir(GL_Scripts);
list = LoadGLS(ScriptFileName, 1);      // parse level .gls (+ all #includes)
ExecuteCommandFile(ConsoleFileName);    // multiplayer only
ConvertParsedObjects(list);             // ToXxx per object; loading bar advances
FreeParsedObjectList(list);
```

Full details in **`level_loading_notes.md`**, including everything `ToMap` does.

Placed actors are **not** `rolename at x,y,z`. The `map` section carries clauses of the
form

```
[extreme] use <RoleRef> in team <N> for "RIF OBJECT" [as "TOKEN"] and "RIF OBJECT2" ...
```

where the quoted names are objects **inside the level `.rif`** - the rif supplies the
position and orientation, the script only binds a role and team to them. These are
stored in a 64-bucket hash embedded in `ParsedMap` at `+0x1b60` (so `ParsedMap` is
`0x1b78` bytes, not `0x1b60`), filled by the ParsedMap `checkValue` override
`CheckValue_Map` @ 0x0047efa0 under pseudo-field id **9**, and drained by the tail of
`ToMap`. Team clauses ("team numbers cannot be negative", "no team specified - assuming
team 0") set the `team` field of each binding, which indexes `TeamSlots` @ 0x007b3ec4.

`InitPlacedActorEntry` @ 0x0047c010 is a *different* thing: a generic tagged list node
(`{vtbl, 9, ptr, ParsedThingBase* thing, ptr}`) built by the grammar. Its vtable slot 7
is `ToGameObject_nop`, so `ConvertParsedObjects` skips it.

## Notes for GkPlus

- The full C++ API for this system lives in `src/GLS.h` / `src/GLS.cpp`
  (`gk::gls`): faithful `ParsedThing` layout, field-id enum, typed accessors,
  section-type discrimination via `parser_func`, plus wrappers for `LoadGLS`,
  `ConvertParsedObjects` and `FreeParsedObjectList`, the programmatic construction
  API (`Create` / `Set` / `InheritFrom` / `RegisterGameObject` / `ToRole`) and the
  runtime reflection above (`SectionFields` / `FindField` / `ResetConversionCache`).
  `src/JsGls.cpp` exposes all of it to scripts as the `gls` namespace, which is how
  a `.gsh` header gets re-implemented as an `.mjs` module.
- Because `ParsedRole` caches the converted `Role*` at +0x1b60, converting a role
  twice returns the same `Role`.
- LoadGLS is destructive global state (`ParsedObjList`, symbol tables): do not call
  concurrently with the game's own loading.

### Programmatic registration (`gk::gls` builder API)

Game objects can also be defined and registered **without a .gls file** by driving
the same machinery directly (see the usage example in `src/GLS.h`):

- `Create(SectionType)` calls the game's own section constructor (`DoParseRole`
  etc.), so all defaults/ranges/allowed-sub-type lists are exactly the script ones.
- `Set(thing, FieldId, value)` goes through the `checkValue` vtable slot @ 0x47bd60:
  the game performs the range check, `none`-allowed check and sub-object ctor-list
  check, copies strings onto **its own heap** (no CRT mismatch between the mod DLL's
  static CRT and the game's), and ref-counts sub-objects. The wrapper clears
  `is_defined[id]` first so re-assignment works (scripts would error
  "defined more than once").
- CheckValue dispatch (jump table @ 0x47bfd0, indexed by FieldType): Boolean stores
  a single byte without range checks; Integer checks int min/max; Float reads a
  double at `ParsedField+8` and checks double min/max; Custom/String handle `none`
  and old-value release.
- `InheritFrom` = the `child : parent` copy (`copyFields` slot).
- `ToRole` / `RegisterGameObject` gate on `isValidDeep` before converting. This is
  load-bearing: the game's own `ToRole` does **not** check sub-object validity and
  writes through the null returned by `ToCharacter` for an incomplete character
  (crash). Fill in every required ("req") field from the tables above before
  registering.
- Registered roles are inserted into the roles hash with a fresh id
  (`next_entity_id`), so they are immediately visible to `GetRoleByName` /
  `gk.roles` and spawnable via `SpawnRole`.
- Converting a parsed `map` loads level geometry; `RegisterGameObject` refuses it.
