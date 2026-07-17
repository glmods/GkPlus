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
| 0x00474920 | `GSHTokenize` | flex lexer |
| 0x00478400 | `ParseGSH` | yacc parser (all grammar actions) |
| 0x004747b0 | `ConvertParsedObjects(ParsedObjectList*)` | Calls `toGameObject` (vtbl slot 7) on every parsed object, releasing refs and updating the loading bar. |
| 0x00474870 | `FreeParsedObjectList(ParsedObjectList*)` | Releases remaining refs and frees the list itself. |
| 0x00477000 | `PrintParseError(fmt, ...)` | Increments error counter |
| 0x00477050 | `PrintParseWarning(fmt, ...)` | |
| 0x00477140 | `PushFileToParserStack(File*)` | `#include` support |
| 0x00477f70 | `PopFileFromParserStack` | |
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
| 0x004ae570 | `GetShape(char* name, char* file)` | Loads shape from a .rif |
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

## Global State (offsets from base)

| Offset | Type | Name |
|--------|------|------|
| 0x007b3cfc | `ParsedObjectList*` | `ParsedObjList` - result list built during pass 2 |
| 0x007b3d00 | int | `g_LoadGLS_Param2` - mode bitmask from `LoadGLS` |
| 0x00739a4c | int | `g_ParsePassCounter` - 1/2 before each pass, 0 while running |
| 0x007399c0 | `ParseData*` | `PreviousScriptFile` - pass-1 replay buffer |
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
| name | 0x00 | S | req |
| replace | 0x69 | B | dflt no |
Converts to a destructibility record of type 4 `{vtbl, 4, char* name, bool replace}`.

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
| blob shadow | 0x5a | I | dflt 0, 0..1 |
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
| `ToShape` | 0x47c260 | `GetShape(name, file)` - loads from .rif |
| `ToHierarchy` | 0x47c390 | hierarchy object from .rif |
| `ToParticleGenerator` | 0x47c750 | `ParticleGenerator` (0xd4) |
| `ToDestructibility` | 0x47e680 | `{vtbl, int type}` (8 bytes) |
| `ToFragData` | 0x47e890 | 0x24-byte record `{vtbl, 3, Role* role, Role* replace_role, char* remove, int scale, bool replace, bool symmetric, float blast_range, float blast_damage}` |
| `ToReplaceDestructibility` | 0x47eaa0 | 0x10-byte record `{vtbl, 4, char* name, bool replace}` |
| `ToAmmo` | 0x47d740 | fills `AmmoInfos` ammo slot `[weapon_type*19 + ammo_type]` (table of `Ammo*` at AmmoInfos+0x180) |
| `ToAmmoInfo` | 0x47d8f0 | fills `AmmoInfos[ammo_type]` (0x14-byte `AmmoInfo` records at +0) |
| `ToCameraTrack` | 0x47d9f0 | camera track object (0xa0) + pgen/pgen2 |
| `ToMap` | 0x47f160 | loads the level geometry itself (huge) |
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

Placed actors (`rolename at x,y,z` entries produced by grammar rule 99 via
`InitPlacedActorEntry`), team clauses ("team numbers cannot be negative", "no team
specified - assuming team 0") and console-variable assignments are part of the same
grammar and travel through the same list; they are consumed during conversion when
the map spawns entities.

## Notes for GkPlus

- The full C++ API for this system lives in `src/GLS.h` / `src/GLS.cpp`
  (`gk::gls`): faithful `ParsedThing` layout, field-id enum, typed accessors,
  section-type discrimination via `parser_func`, plus wrappers for `LoadGLS`,
  `ConvertParsedObjects` and `FreeParsedObjectList`.
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
