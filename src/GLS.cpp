#include "GLS.h"

#include "Console.h"
#include "Core.h"
#include "Memory.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h> // before detours.h, which needs its architecture macros

#include <detours.h>

// Not DetourUtils.h: its gk::DetourAttach overloads are for __thiscall member
// pointers, and merely declaring them inside namespace gk hides the global
// templates that handle a plain function pointer.

#include <cstdio>
#include <cstring>
#include <iterator>

namespace gk::gls {
namespace {
// Section entry functions; ParsedThing::parser_func holds one of these.
struct {
  FastCall<ParsedObjectList *, const char *, int> LoadGLS;
  FastCall<void, ParsedObjectList *> ConvertParsedObjects;
  FastCall<void, ParsedObjectList *> FreeParsedObjectList;
  ThisCall<Role *, ParsedThing *> ToRole;

  void *ParseShape;
  void *ParseHierarchy;
  void *ParseParticleGenerator;
  void *ParseLight;
  void *ParseProjectile;
  void *ParseDestructibility;
  void *ParseFragData;
  void *ParseUnk2;
  void *DoParseRole;
  void *DoParseCharacter;
  void *DoParseAmmo;
  void *DoParseAmmoInfo;
  void *DoParseCameraTrack;
  void *DoParseMap;
  void *DoParseDirectory;
} Game;

void EnsureResolved() {
  static bool resolved = [] {
    GetObjectAtOffset(Game.LoadGLS, 0x00474540);
    GetObjectAtOffset(Game.ConvertParsedObjects, 0x004747b0);
    GetObjectAtOffset(Game.FreeParsedObjectList, 0x00474870);
    GetObjectAtOffset(Game.ToRole, 0x0047cc10);

    GetObjectAtOffset(Game.ParseShape, 0x0047c1b0);
    GetObjectAtOffset(Game.ParseHierarchy, 0x0047c290);
    GetObjectAtOffset(Game.ParseParticleGenerator, 0x0047c3c0);
    GetObjectAtOffset(Game.ParseLight, 0x0047e050);
    GetObjectAtOffset(Game.ParseProjectile, 0x0047e2f0);
    GetObjectAtOffset(Game.ParseDestructibility, 0x0047e5d0);
    GetObjectAtOffset(Game.ParseFragData, 0x0047e6c0);
    GetObjectAtOffset(Game.ParseUnk2, 0x0047e9e0);
    GetObjectAtOffset(Game.DoParseRole, 0x0047cb90);
    GetObjectAtOffset(Game.DoParseCharacter, 0x0047db00);
    GetObjectAtOffset(Game.DoParseAmmo, 0x0047d6c0);
    GetObjectAtOffset(Game.DoParseAmmoInfo, 0x0047d870);
    GetObjectAtOffset(Game.DoParseCameraTrack, 0x0047d970);
    GetObjectAtOffset(Game.DoParseMap, 0x0047ed90);
    GetObjectAtOffset(Game.DoParseDirectory, 0x00466ba0);
    return true;
  }();
  (void)resolved;
}
} // namespace

SectionType ParsedThing::type() const {
  EnsureResolved();
  if (parser_func == Game.ParseShape)
    return SectionType::Shape;
  if (parser_func == Game.ParseHierarchy)
    return SectionType::Hierarchy;
  if (parser_func == Game.ParseParticleGenerator)
    return SectionType::ParticleGenerator;
  if (parser_func == Game.ParseLight)
    return SectionType::Light;
  if (parser_func == Game.ParseProjectile)
    return SectionType::Projectile;
  if (parser_func == Game.ParseDestructibility)
    return SectionType::Destructibility;
  if (parser_func == Game.ParseFragData)
    return SectionType::FragData;
  if (parser_func == Game.ParseUnk2)
    return SectionType::ReplaceDestructibility;
  if (parser_func == Game.DoParseRole)
    return SectionType::Role;
  if (parser_func == Game.DoParseCharacter)
    return SectionType::Character;
  if (parser_func == Game.DoParseAmmo)
    return SectionType::Ammo;
  if (parser_func == Game.DoParseAmmoInfo)
    return SectionType::AmmoInfo;
  if (parser_func == Game.DoParseCameraTrack)
    return SectionType::CameraTrack;
  if (parser_func == Game.DoParseMap)
    return SectionType::Map;
  if (parser_func == Game.DoParseDirectory)
    return SectionType::Directory;
  return SectionType::Unknown;
}

ParsedObjectList *LoadGLS(const char *file, int mode) {
  EnsureResolved();
  return Game.LoadGLS(file, mode);
}

// --- Parsing from memory --------------------------------------------------------

namespace {
// The parser's file-backed input source: the game's `File`, vtbl @ 0x00652904.
// LoadGLS builds it inline, so there is no constructor to call and no dtor to
// reproduce - the vtable's own slot 0 owns every field below. See the header for
// why a null FILE * plus a text tail is a complete in-memory source.
struct ParserSource {
  void *vtbl;
  void *gls_file;    // FILE *, from _fopen. Null is not fatal.
  char *text_buffer; // pool memory, freed by File::Dtor @ 0x004779f0
  char *text_cursor; // read cursor into text_buffer
  char *file_name;   // pool memory, freed by File::Dtor
};
static_assert(sizeof(ParserSource) == 0x14);

// Resolved into a trampoline by DetourAttach, so calling through this runs the
// original.
FastCall<void, ParserSource *> PushFileToParserStack;
// __cdecl, and it must be the *game's* fclose: the FILE * came from the game's CRT,
// which is not this DLL's.
CDecl<int, void *> CloseFile;

// Armed by SourceTextScope, consumed by the first matching push. Not thread-safe,
// and does not need to be: the parser is main-thread-only and its global state
// already says so.
struct {
  const char *name = nullptr;
  const char *text = nullptr;
} Armed;

// Replaces the 2-byte "\n" LoadGLS allocated with the whole script, so the parser
// reads `text` and then hits EOF.
bool InstallSourceText(ParserSource *source, const char *text) {
  size_t len = std::strlen(text);
  // +2, not +1: room to append the trailing newline the buffer being replaced
  // existed to supply.
  char *buffer = static_cast<char *>(pool_alloc(len + 2));
  if (!buffer) {
    return false;
  }
  std::memcpy(buffer, text, len);
  if (len == 0 || buffer[len - 1] != '\n') {
    buffer[len++] = '\n';
  }
  buffer[len] = '\0';

  // Pool memory both ways: File::Dtor frees text_buffer with the game's free, and
  // this DLL's ::free would be the wrong heap entirely (see Memory.h).
  pool_free(source->text_buffer);
  source->text_buffer = buffer;
  source->text_cursor = buffer;

  // A display name is not a path, so this should always be null already. If a file
  // of that name does happen to exist, its bytes would be read *before* ours -
  // close it, so the text is the whole input either way.
  if (source->gls_file) {
    CloseFile(source->gls_file);
    source->gls_file = nullptr;
  }
  return true;
}

void __fastcall HookedPushFileToParserStack(ParserSource *source) {
  if (Armed.text && source && source->file_name &&
      std::strcmp(source->file_name, Armed.name) == 0 &&
      InstallSourceText(source, Armed.text)) {
    // One-shot: whatever the text #includes must reach the real file.
    Armed.name = nullptr;
    Armed.text = nullptr;
  }
  PushFileToParserStack(source);
}
} // namespace

SourceTextScope::SourceTextScope(const char *display_name, const char *source)
    : previous_name_{Armed.name}, previous_text_{Armed.text} {
  if (display_name && source) {
    Armed.name = display_name;
    Armed.text = source;
  }
}

SourceTextScope::~SourceTextScope() {
  Armed.name = previous_name_;
  Armed.text = previous_text_;
}

ParsedObjectList *ParseSource(const char *source, const char *display_name,
                              int mode) {
  if (!source || !display_name) {
    return nullptr;
  }
  SourceTextScope armed{display_name, source};
  return LoadGLS(display_name, mode);
}

GlsSystem::GlsSystem() {
  GetObjectAtOffset(PushFileToParserStack, 0x00477140);
  GetObjectAtOffset(CloseFile, 0x005f0127);
  DetourAttach(&PushFileToParserStack, HookedPushFileToParserStack);
}

GlsSystem::~GlsSystem() {
  DetourDetach(&PushFileToParserStack, HookedPushFileToParserStack);
}

void ConvertParsedObjects(ParsedObjectList *list) {
  EnsureResolved();
  Game.ConvertParsedObjects(list);
}

void FreeParsedObjectList(ParsedObjectList *list) {
  EnsureResolved();
  Game.FreeParsedObjectList(list);
}

Role *ToRole(ParsedThing *thing) {
  EnsureResolved();
  if (!thing || thing->type() != SectionType::Role)
    return nullptr;
  // ToRole null-derefs on incomplete sub-objects (e.g. a character missing a
  // required field makes ToCharacter return null, which ToRole then writes
  // through), so gate on deep validity ourselves.
  if (!thing->vtbl->is_valid_deep(thing))
    return nullptr;
  return Game.ToRole(thing);
}

namespace {
void *EntryFor(SectionType type) {
  switch (type) {
  case SectionType::Shape:
    return Game.ParseShape;
  case SectionType::Hierarchy:
    return Game.ParseHierarchy;
  case SectionType::ParticleGenerator:
    return Game.ParseParticleGenerator;
  case SectionType::Light:
    return Game.ParseLight;
  case SectionType::Projectile:
    return Game.ParseProjectile;
  case SectionType::Destructibility:
    return Game.ParseDestructibility;
  case SectionType::FragData:
    return Game.ParseFragData;
  case SectionType::ReplaceDestructibility:
    return Game.ParseUnk2;
  case SectionType::Role:
    return Game.DoParseRole;
  case SectionType::Character:
    return Game.DoParseCharacter;
  case SectionType::Ammo:
    return Game.DoParseAmmo;
  case SectionType::AmmoInfo:
    return Game.DoParseAmmoInfo;
  case SectionType::CameraTrack:
    return Game.DoParseCameraTrack;
  case SectionType::Map:
    return Game.DoParseMap;
  case SectionType::Directory:
    return Game.DoParseDirectory;
  default:
    return nullptr;
  }
}

// Clears the duplicate-definition flag (so programmatic re-assignment is
// allowed where scripts would error) and runs the game's CheckValue.
bool ApplyField(ParsedThing &thing, FieldId id, ParsedValue value) {
  auto index = static_cast<size_t>(id);
  ParsedField field{nullptr, static_cast<int32_t>(id), value};
  thing.is_defined[index] = false;
  thing.vtbl->check_value(&thing, &field);
  return thing.is_defined[index];
}
} // namespace

ParsedThing *Create(SectionType type) {
  EnsureResolved();
  auto *entry = reinterpret_cast<CDecl<ParsedThing *>>(EntryFor(type));
  return entry ? entry() : nullptr;
}

void Release(ParsedThing *thing) {
  if (!thing)
    return;
  if (--thing->ref_count == 0)
    thing->vtbl->dtor(thing, 1);
}

bool InheritFrom(ParsedThing &thing, const ParsedThing &parent) {
  if (thing.parser_func != parent.parser_func)
    return false;
  thing.vtbl->copy_fields(&thing, &parent);
  return true;
}

bool Set(ParsedThing &thing, FieldId id, bool value) {
  if (thing.field_type(id) != FieldType::Boolean)
    return false;
  ParsedValue v{};
  v.integer = value ? 1 : 0;
  return ApplyField(thing, id, v);
}

bool Set(ParsedThing &thing, FieldId id, int32_t value) {
  if (thing.field_type(id) != FieldType::Integer)
    return false;
  ParsedValue v{};
  v.integer = value;
  return ApplyField(thing, id, v);
}

bool Set(ParsedThing &thing, FieldId id, double value) {
  if (thing.field_type(id) != FieldType::Float)
    return false;
  ParsedValue v{};
  v.flt = value;
  return ApplyField(thing, id, v);
}

bool Set(ParsedThing &thing, FieldId id, const char *value) {
  if (thing.field_type(id) != FieldType::String)
    return false;
  ParsedValue v{};
  v.string = const_cast<char *>(value); // CheckValue copies onto the game heap
  return ApplyField(thing, id, v);
}

bool Set(ParsedThing &thing, FieldId id, ParsedThing *object) {
  if (thing.field_type(id) != FieldType::Custom)
    return false;
  ParsedValue v{};
  v.object = object;
  return ApplyField(thing, id, v);
}

bool SetNone(ParsedThing &thing, FieldId id) {
  switch (thing.field_type(id)) {
  case FieldType::String:
    return Set(thing, id, static_cast<const char *>(nullptr));
  case FieldType::Custom:
    return Set(thing, id, static_cast<ParsedThing *>(nullptr));
  default:
    return false;
  }
}

bool IsValid(const ParsedThing &thing) {
  auto &mut = const_cast<ParsedThing &>(thing);
  return mut.vtbl->is_valid(&mut);
}

bool IsValidDeep(const ParsedThing &thing) {
  auto &mut = const_cast<ParsedThing &>(thing);
  return mut.vtbl->is_valid_deep(&mut);
}

// --- Reflection ---------------------------------------------------------------

namespace {
struct SectionName {
  SectionType type;
  const char *name;
};

// The grammar's section keywords. `ReplaceDestructibility` is the one without a
// keyword of its own - it is the `name` + `replace` form of a destructibility
// block (ParseUnk2 @ 0x0047e9e0) - so it gets a descriptive name instead.
const SectionName SectionNames[] = {
    {SectionType::Shape, "shape"},
    {SectionType::Hierarchy, "hierarchy"},
    {SectionType::ParticleGenerator, "pgenerator"},
    {SectionType::Light, "light"},
    {SectionType::Projectile, "projectile"},
    {SectionType::Destructibility, "destructibility"},
    {SectionType::FragData, "frag data"},
    {SectionType::ReplaceDestructibility, "replace destructibility"},
    {SectionType::Role, "role"},
    {SectionType::Character, "character"},
    {SectionType::Ammo, "ammo"},
    {SectionType::AmmoInfo, "ammo info"},
    {SectionType::CameraTrack, "camera track"},
    {SectionType::Map, "map"},
    {SectionType::Directory, "directory"},
};

char FoldChar(char c) {
  if (c == '_')
    return ' '; // "walking_speed" and "walking speed" are the same key
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

bool NameMatches(const char *a, const char *b) {
  if (!a || !b)
    return false;
  for (; *a && *b; ++a, ++b) {
    if (FoldChar(*a) != FoldChar(*b))
      return false;
  }
  return *a == *b;
}

// Reads the schema straight off a throwaway instance. Built on first use and
// kept for the process: a section constructor is cheap but not free, and the
// answer never changes.
std::vector<FieldInfo> BuildFields(SectionType type) {
  std::vector<FieldInfo> fields;
  ParsedThing *probe = Create(type);
  if (!probe)
    return fields;
  for (size_t i = 0; i < NumFields; ++i) {
    FieldType ft = probe->field_types[i];
    if (ft == FieldType::None)
      continue; // this section does not accept the id at all
    FieldInfo info{};
    info.id = static_cast<FieldId>(i);
    info.name = probe->field_names[i];
    info.type = ft;
    // field_satisfied starts true exactly for the fields that have a default;
    // a required field starts false and only IsValid ever complains about it.
    info.optional = probe->field_satisfied[i];
    // For String and Custom the min slot is not a bound at all - the ctor puts
    // "none is allowed" there, which is what CheckValue tests before storing null.
    info.none_ok = (ft == FieldType::String || ft == FieldType::Custom) &&
                   probe->min_values[i].boolean;
    info.min_integer = probe->min_values[i].integer;
    info.max_integer = probe->max_values[i].integer;
    info.min_float = probe->min_values[i].flt;
    info.max_float = probe->max_values[i].flt;
    fields.push_back(info);
  }
  Release(probe);
  return fields;
}
} // namespace

const char *SectionTypeName(SectionType type) {
  for (const SectionName &entry : SectionNames) {
    if (entry.type == type)
      return entry.name;
  }
  return "unknown";
}

SectionType SectionTypeFromName(const char *name) {
  for (const SectionName &entry : SectionNames) {
    if (NameMatches(entry.name, name))
      return entry.type;
  }
  return SectionType::Unknown;
}

const SectionType *AllSectionTypes() {
  static SectionType types[std::size(SectionNames) + 1] = {};
  static bool filled = [] {
    size_t i = 0;
    for (const SectionName &entry : SectionNames)
      types[i++] = entry.type;
    types[i] = SectionType::Unknown;
    return true;
  }();
  (void)filled;
  return types;
}

const std::vector<FieldInfo> &SectionFields(SectionType type) {
  static std::vector<FieldInfo> cache[static_cast<size_t>(SectionType::Directory) + 1];
  static bool built[static_cast<size_t>(SectionType::Directory) + 1] = {};
  auto index = static_cast<size_t>(type);
  if (index >= std::size(cache)) {
    static const std::vector<FieldInfo> empty;
    return empty;
  }
  if (!built[index]) {
    built[index] = true;
    cache[index] = BuildFields(type);
  }
  return cache[index];
}

const FieldInfo *FindField(SectionType type, const char *name) {
  for (const FieldInfo &field : SectionFields(type)) {
    if (NameMatches(field.name, name))
      return &field;
  }
  return nullptr;
}

namespace {
// A value that satisfies `field` without tripping its bounds. Only needed to make
// the section *complete*: an object missing any required field is quietly demoted
// to abstract ("abstract definition not declared with 'abstract'") and then left
// OUT of ParsedObjList, so the probe would have nothing to read - which is exactly
// what the first version of this hit, with LoadGLS reporting "empty script found".
std::string FillerFor(const FieldInfo &field) {
  auto clamp_int = [&](int32_t v) {
    return v < field.min_integer ? field.min_integer
                                 : (v > field.max_integer ? field.max_integer : v);
  };
  switch (field.type) {
  case FieldType::Boolean:
    return "no";
  case FieldType::Integer: {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", clamp_int(0));
    return buf;
  }
  case FieldType::Float: {
    double v = 0.0;
    if (v < field.min_float) {
      v = field.min_float;
    }
    if (v > field.max_float) {
      v = field.max_float;
    }
    char buf[64];
    // %.6f, not %g: the lexer has no exponent form, so 1e-05 is a syntax error.
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return buf;
  }
  case FieldType::String:
    return field.none_ok ? "none" : "\"gkplus\"";
  case FieldType::Custom:
    // No section has a required Custom field, so this only ever runs for one that
    // accepts `none`; if that changes it will show up as a failed probe rather
    // than a wrong answer.
    return "none";
  default:
    return {};
  }
}
} // namespace

int TryParse(const char *source) {
  if (!source) {
    return -1;
  }
  ParsedObjectList *list = ParseSource(source, "<gkplus try_parse>");
  if (!list) {
    return -1;
  }
  int count = list->n_entries;
  FreeParsedObjectList(list);
  return count;
}

bool ProbeKeywords(SectionType section, FieldId field,
                   const std::vector<std::string> &keywords,
                   std::vector<std::optional<int32_t>> *out) {
  if (!out) {
    return false;
  }
  out->assign(keywords.size(), std::nullopt);
  const char *section_name = SectionTypeName(section);
  const FieldInfo *info = nullptr;
  for (const FieldInfo &f : SectionFields(section)) {
    if (f.id == field) {
      info = &f;
      break;
    }
  }
  // Reflection first: asking for a field the section does not accept would
  // produce a script the parser rejects wholesale, and a confusing empty result.
  if (!info || !info->name) {
    return false;
  }

  // Precomputed once: every required field except the one under test, plus every
  // optional *numeric* one.
  //
  // The required ones are what makes the section complete. The optional numeric
  // ones are only there to silence "default value assumed for '%s'", which
  // otherwise prints once per unset field per section - tens of thousands of
  // console lines across a full run. They are safe to fill because the value is
  // clamped into the field's own declared range; optional String and Custom
  // fields are left alone, since a filler there could be rejected outright and
  // cost the whole parse.
  std::string filler;
  // `life` is required by a pgenerator but invisible to reflection: it is field
  // id 0x42, and the section's CheckValue override intercepts it before the
  // normal machinery, storing it in the 0x1b70 object's extension instead of
  // parsed_values - so field_types[0x42] is never set and SectionFields cannot
  // see it. Without it every probe object is silently demoted to abstract.
  if (section == SectionType::ParticleGenerator && field != FieldId::Count) {
    filler += "\tlife infinite\n";
  }
  for (const FieldInfo &f : SectionFields(section)) {
    if (f.id == field || !f.name) {
      continue;
    }
    // ONLY required fields. Optional ones exist to be omitted - the sole cost is
    // a "default value assumed" warning - and every one emitted is another chance
    // to hit a syntax error that poisons the whole run.
    //
    // Filling them was originally a noise-reduction measure and it cost two runs:
    // first via optional *Integer* enums, where no shipped script ever writes a
    // bare number and the grammar wants a keyword (`secondary weapon 0`), and
    // then again via some optional Float or Boolean in the character section. The
    // noise is cosmetic; the correctness is not.
    if (f.optional) {
      continue;
    }
    std::string value = FillerFor(f);
    if (!value.empty()) {
      filler += "\t";
      filler += f.name;
      filler += " ";
      filler += value;
      filler += "\n";
    }
  }

  // ONE keyword per parse, not one file for all of them. An unrecognised keyword
  // is a *syntax error*, not a per-value rejection, and the yacc parser abandons
  // the rest of the file - so a single bad name in a batch loses every keyword
  // after it. Parsing each separately costs a few dozen tiny parses and makes a
  // rejection mean exactly "this keyword is not valid for this field".
  //
  // The label carries no digits on purpose: `GkPlusProbe0` was itself a syntax
  // error for some sections, so the identifier rule appears not to accept them.
  auto slot = static_cast<size_t>(field);
  bool any = false;
  bool reported = false;
  for (size_t i = 0; i < keywords.size(); ++i) {
    std::string script = std::string{section_name} + " GkPlusProbe\n{\n\t" +
                         info->name + " " + keywords[i] + "\n" + filler + "}\n";
    ParsedObjectList *list = ParseSource(script.c_str(), "<gkplus probe>");
    bool got = false;
    if (list) {
      for (ParsedThing *thing : *list) {
        // `is_defined` separates "the parser accepted this keyword" from "the
        // section kept the field's default".
        if (thing && thing->is_defined[slot]) {
          (*out)[i] = thing->values[slot].integer;
          any = true;
          got = true;
          break;
        }
      }
      FreeParsedObjectList(list);
    }
    // STOP at the first failure. A syntax error does not just lose that keyword:
    // it poisons the parser for every subsequent LoadGLS call in the process.
    // LoadGLS resets its error counter, ParsedObjList and the symbol tables, but
    // evidently not the file stack, and nothing recovers - a verbatim copy of a
    // shipped section fails just the same afterwards. Carrying on would report
    // every later keyword as "rejected" when the truth is "never tested", which
    // is worse than stopping.
    if (!got) {
      reported = true;
      std::string why = "gls probe: stopped at '" + keywords[i] +
                        "' - a failed parse poisons every later one, so the "
                        "remaining keywords are untested, not rejected. Script:";
      Print(why.c_str());
      DebugWrite(why);
      Print(script.c_str());
      DebugWrite(script);
      break;
    }
  }
  (void)reported;
  return any;
}

void ResetConversionCache(ParsedThing *thing) {
  if (!thing || thing->type() != SectionType::Role)
    return;
  // The dword immediately past the 0x1b60 base - a role allocates 0x1b68 for it.
  auto *cache = reinterpret_cast<void **>(reinterpret_cast<char *>(thing) +
                                          sizeof(ParsedThing));
  *cache = nullptr;
}

void *RegisterGameObject(ParsedThing &thing) {
  EnsureResolved();
  auto type = thing.type();
  if (type == SectionType::Unknown || type == SectionType::Map)
    return nullptr;
  if (type == SectionType::Role)
    return ToRole(&thing);
  if (!thing.vtbl->is_valid_deep(&thing))
    return nullptr;
  // Every non-role converter returns void; discard the register's garbage.
  thing.vtbl->to_game_object(&thing);
  return nullptr;
}
} // namespace gk::gls
