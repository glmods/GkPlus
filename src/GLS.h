#pragma once

#include "Core.h"
#include "List.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// C++ API over Gunlok's GLS/GSH script parser.
// See gls_system_notes.md for the full reverse-engineering reference.

namespace gk {
struct Role;
}

namespace gk::gls {
// Section type of a parsed object, discriminated via ParsedThing::parser_func.
enum class SectionType {
  Unknown,
  Shape,
  Hierarchy,
  ParticleGenerator,
  Light,
  Projectile,
  Destructibility,
  FragData,
  ReplaceDestructibility, // the "name + replace" destructibility variant
  Role,
  Character,
  Ammo,
  AmmoInfo,
  CameraTrack,
  Map,
  Directory,
};

enum class FieldType : int32_t {
  None = 0,
  Boolean = 1,
  Integer = 2,
  Float = 3,
  Custom = 4, // sub-object (ParsedThing*)
  String = 5,
};

// Global field ids shared by every section type (0x00..0x88).
// Which ids a section accepts is listed in gls_system_notes.md.
enum class FieldId : int32_t {
  Name = 0x00,
  File = 0x01,
  Bitmap = 0x02,
  Hotspot = 0x03,
  AlternateHotspot = 0x04,
  Shape = 0x05,
  InventoryShape = 0x06,
  Pgen = 0x07,
  Pgen2 = 0x08,
  Character = 0x0a,
  Projectile = 0x0b,
  WalkingSpeed = 0x0c,
  TurningSpeed = 0x0d,
  ScanDelay = 0x0e,
  ScanAcceptanceAngle = 0x0f,
  AngularScanRate = 0x10,
  MineLayingTime = 0x11,
  MagazineSize = 0x12,
  SalvoSize = 0x13,
  RoundTime = 0x14,
  ReloadTime = 0x15,
  LifeTimer = 0x16,
  AmmoType = 0x17,
  WeaponType = 0x18, // "weapon" in character sections
  SecondaryWeapon = 0x19,
  Description = 0x1a,
  PickupName = 0x1b,
  AmmoName = 0x1c,
  MaxPerSlot = 0x1d,
  Light = 0x1e,
  HitLight = 0x1f,
  Sound = 0x20,
  Red = 0x21,
  Green = 0x22,
  Blue = 0x23,
  Alpha = 0x24,
  SpecularRed = 0x25,
  SpecularGreen = 0x26,
  SpecularBlue = 0x27,
  Range = 0x28, // "blast range" in projectile/frag data
  Strength = 0x29,
  Damage = 0x2a,
  DamageMultiplier = 0x2b,
  BlastDamage = 0x2c,
  MaxRange = 0x2d,
  ShotSpeedMultiplier = 0x2e, // "firing speed" in ammo sections
  TargetCycleTime = 0x2f,
  WeaponCycleTime = 0x30,
  WeaponCycleTime2 = 0x31,
  AlarmDelay = 0x32,
  Gravity = 0x33,
  Aim = 0x34,
  SightAngle = 0x35,
  GunYawAngle = 0x36,
  ElevationAngle = 0x37,
  SightRange = 0x38,
  HearingRange = 0x39,
  AlertRadius = 0x3a,
  Aggression = 0x3b,
  CanTurn = 0x3c,
  DrawVisionCone = 0x3d,
  DrawHearingRange = 0x3e,
  StatusWindowU = 0x3f,
  StatusWindowV = 0x40,
  Type = 0x41,
  Rate = 0x43,
  X = 0x44,
  Y = 0x45,
  Z = 0x46,
  Identifier = 0x47,
  SeverPoint = 0x48,
  Ai = 0x49,
  InterfaceBeamDelay = 0x4a,
  InterfaceBeamEffect = 0x4b,
  InterfaceBeamScript = 0x4c,
  InterfaceBeamDuration = 0x4d,
  Resistance = 0x4e,
  ResistanceFactor = 0x4f,
  CameraPlane = 0x50,
  MaxCameraDistance = 0x51,
  MaxCameraFocusHeight = 0x52,
  MinCameraFocusHeight = 0x53,
  ShadowObjectRif = 0x54,
  ShadowObjectName = 0x55,
  AlphaFogging = 0x56,
  Reflective = 0x57,
  PerVertexFogging = 0x58,
  Destructibility = 0x59,
  BlobShadow = 0x5a,
  Armour = 0x5b,
  Shields = 0x5c,
  ActionOnDeath = 0x5d,
  PickupRadius = 0x5e,
  RechargeRate = 0x5f,
  FragRole = 0x60,
  ReplaceRole = 0x61,
  Remove = 0x62,
  Scale = 0x63,
  StartScale = 0x64,
  EndScale = 0x65,
  Spin = 0x66,
  ParticleTTL = 0x67,
  GenerateGenerators = 0x68,
  Replace = 0x69,
  Symmetric = 0x6a,
  Radius = 0x6b,
  Height = 0x6c,
  Size = 0x6d,
  DestinationSelectable = 0x6e,
  DestroyAfterCollection = 0x6f,
  MovesOnLifts = 0x70,
  StatusDisplay = 0x71,
  HitTestIgnore = 0x72,
  NoLighting = 0x73,
  AlwaysCpuControlled = 0x74,
  FragControl = 0x75,
  CustomisationHierarchy = 0x76,
  ShadowHierarchy = 0x77,
  Limit = 0x78,
  SwitchSize = 0x79,
  MaxVerticesPerSection = 0x7a,
  GenerationLimit = 0x7b,
  Alertable = 0x7c,
  LatchTrigger = 0x7d,
  ReconName = 0x7e,
  ReconAiShort = 0x7f,
  ReconAiNumber = 0x80,
  ReconAiLong = 0x81,
  ReconAiLong2 = 0x82,
  MaxWeapon = 0x83,
  MaxAmmo = 0x84,
  MaxModule = 0x85,
  InitialFirstPersonRange = 0x86,
  MaximumFirstPersonRange = 0x87,
  MetaSound = 0x88,

  Count = 0x89,
};

inline constexpr size_t NumFields = static_cast<size_t>(FieldId::Count);

struct ParsedThing;

// 8-byte value slot. Interpretation depends on the field's FieldType:
// Boolean/Integer use integer, Float uses flt (double), String is a heap copy,
// Custom is a ref-counted ParsedThing*.
union ParsedValue {
  bool boolean;
  int32_t integer;
  double flt;
  ParsedThing *object;
  char *string;
};
static_assert(sizeof(ParsedValue) == 8);

// List of the allowed sub-object constructors of a Custom field.
using ComplexParsedValue = List<void *>;
static_assert(sizeof(ComplexParsedValue) == 0x10);

// Argument to the checkValue vtable slot: a single field assignment.
struct ParsedField {
  void *vtbl;
  int32_t id;
  ParsedValue value;
};
static_assert(offsetof(ParsedField, value) == 0x8);
static_assert(sizeof(ParsedField) == 0x10);

struct ParsedThingVtbl {
  ThisCall<void *, ParsedThing *, unsigned char> dtor; // flags & 1: free memory
  ThisCall<bool, ParsedThing *> is_valid;      // all required fields assigned
  ThisCall<bool, ParsedThing *> is_valid_deep; // ... recursively in sub-objects
  ThisCall<int, ParsedThing *, int> filter;    // LoadGLS mode bitmask check
  ThisCall<void, ParsedThing *, const ParsedThing *> copy_fields; // inheritance
  ThisCall<void, ParsedThing *, ParsedField *> check_value; // validate + assign
  ThisCall<void, ParsedThing *> register_object;            // no-op everywhere
  ThisCall<void *, ParsedThing *> to_game_object; // convert into live game data
};
static_assert(sizeof(ParsedThingVtbl) == 0x20);

// One parsed script object (any section type). Allocated by the game;
// 0x1b60 bytes (roles: 0x1b68, the extra dword caches the converted Role*).
//
// ParsedThingBase_Dtor @ 0x0047bae0 walks field_types and pool-frees the String
// slots while refcount-releasing the Custom ones. Neither can be a
// pool_unique_ptr: they share a `ParsedValue` union slot, which a move-only
// member would make non-trivial, and the discriminator is a parallel array.
struct ParsedThing {
  ParsedThingVtbl *vtbl;
  void *symbol_link;
  uint8_t flags; // bit 0: declared 'abstract'
  uint8_t finalized;
  uint16_t pad_0xa;
  int32_t ref_count;
  void *parser_func; // section entry function: the runtime type tag
  FieldType field_types[NumFields];
  ParsedValue values[NumFields];
  ParsedValue min_values[NumFields]; // String/Custom: .boolean = "none allowed"
  ParsedValue max_values[NumFields];
  ComplexParsedValue complex_fields[NumFields];
  // Initialized by the section ctor; set on assignment. ctor-true fields are
  // optional (warn "default value assumed"), ctor-false fields are required.
  bool field_satisfied[NumFields];
  bool field_has_value[NumFields]; // assigned or inherited
  bool is_defined[NumFields];      // assigned in this object (dup detection)
  uint8_t pad_0x193b;
  const char *field_names[NumFields];

  SectionType type() const;

  bool is_abstract() const { return (flags & 1) != 0; }

  FieldType field_type(FieldId id) const {
    return field_types[static_cast<size_t>(id)];
  }

  bool has(FieldId id) const {
    return field_has_value[static_cast<size_t>(id)];
  }

  const char *field_name(FieldId id) const {
    return field_names[static_cast<size_t>(id)];
  }

  std::optional<bool> get_bool(FieldId id) const {
    if (field_type(id) != FieldType::Boolean)
      return std::nullopt;
    return values[static_cast<size_t>(id)].boolean;
  }

  std::optional<int32_t> get_int(FieldId id) const {
    if (field_type(id) != FieldType::Integer)
      return std::nullopt;
    return values[static_cast<size_t>(id)].integer;
  }

  std::optional<double> get_float(FieldId id) const {
    if (field_type(id) != FieldType::Float)
      return std::nullopt;
    return values[static_cast<size_t>(id)].flt;
  }

  std::optional<std::string_view> get_string(FieldId id) const {
    if (field_type(id) != FieldType::String)
      return std::nullopt;
    auto *s = values[static_cast<size_t>(id)].string;
    if (!s)
      return std::nullopt;
    return std::string_view{s};
  }

  ParsedThing *get_object(FieldId id) const {
    if (field_type(id) != FieldType::Custom)
      return nullptr;
    return values[static_cast<size_t>(id)].object;
  }
};
static_assert(offsetof(ParsedThing, parser_func) == 0x10);
static_assert(offsetof(ParsedThing, field_types) == 0x14);
static_assert(offsetof(ParsedThing, values) == 0x238);
static_assert(offsetof(ParsedThing, min_values) == 0x680);
static_assert(offsetof(ParsedThing, max_values) == 0xac8);
static_assert(offsetof(ParsedThing, complex_fields) == 0xf10);
static_assert(offsetof(ParsedThing, field_satisfied) == 0x17a0);
static_assert(offsetof(ParsedThing, field_has_value) == 0x1829);
static_assert(offsetof(ParsedThing, is_defined) == 0x18b2);
static_assert(offsetof(ParsedThing, field_names) == 0x193c);
static_assert(sizeof(ParsedThing) == 0x1b60);

// Node of the parsed-object list. Placed-actor entries (from level scripts)
// use the same node layout but their payload is not a ParsedThing; check
// thing->type() != SectionType::Unknown before using field accessors.
using ParsedObjectNode = List_Member<ParsedThing *>;

// Result of LoadGLS.
using ParsedObjectList = List<ParsedThing *>;
static_assert(sizeof(ParsedObjectNode) == 0x10);
static_assert(sizeof(ParsedObjectList) == 0x10);

// Parses a .gls/.gsh file (two passes, follows #include). Returns the global
// parsed-object list, or nullptr on fatal errors. `mode` is the filter bitmask
// passed to each section's filter vtable slot; the game always uses 1.
// NOTE: uses destructive global parser state - never call while the game is
// itself loading a level.
ParsedObjectList *LoadGLS(const char *file, int mode = 1);

// --- Parsing from memory --------------------------------------------------------
//
// The parser does not need a file. It takes its input through a source object with
// a 3-slot vtable {dtor, Read, GetFileName} - the file-backed one is `File`
// @ vtbl 0x00652904, and the *second pass* already uses a memory-backed
// implementation of the same shape (`ReadRecordedText` @ 0x004770a0, no FILE * in
// sight). Better still, `File` is itself a hybrid: File::ReadFile @ 0x00477ac0
// reads its FILE * and then TOPS UP from an in-memory tail whenever that read came
// up short, which is how LoadGLS guarantees a trailing newline for a file that
// lacks one (it puts a 2-byte "\n" there).
//
// So a File whose FILE * is null and whose tail is a whole script feeds the parser
// entirely from memory, using the game's own class. Two measurements make that
// safe rather than merely clever:
//
//   * a null FILE * is NOT fatal. LoadGLS reports it through PrintParseError,
//     which increments ParseErrorCount @ 0x00739a38 - and that global has no
//     readers anywhere in the binary. What poisons the parser after a syntax error
//     is the un-reset file stack, not this counter.
//   * PushFileToParserStack @ 0x00477140 is the only seam between LoadGLS building
//     the File and ParseGSH consuming it, and it takes the source as its only
//     argument. Five call sites, all inside the parser.
//
// `display_name` is what the parser calls the source in its diagnostics, and it is
// a NAME, not a path: nothing opens it. It does end up inside a synthesized
// '# line 1 "<name>"' directive that pass 2 re-lexes, so it must not contain a
// double quote. Backslashes are fine (every shipped script has paths in strings).
//
// Same hazards as LoadGLS otherwise: destructive global parser state, one thread,
// never during a level load, and a syntax error poisons every later parse.
ParsedObjectList *ParseSource(const char *source, const char *display_name,
                              int mode = 1);

// The arming half of ParseSource, for the one caller that cannot use it: a hook on
// LoadGLS itself, which has to reach the original through its own Detours
// trampoline rather than through gls::LoadGLS (that holds the raw address and
// would re-enter the hook). Arm, then call the trampoline:
//
//   gls::SourceTextScope armed{file, source};
//   return LoadGLS(file, mode);   // the hook's trampoline
//
// The arming is one-shot - it is consumed by the first pushed source whose name
// matches, so an #include inside the text cannot be hijacked - and scoped, so a
// name that never arrives leaves nothing behind.
class SourceTextScope {
public:
  SourceTextScope(const char *display_name, const char *source);
  ~SourceTextScope();
  SourceTextScope(const SourceTextScope &) = delete;
  SourceTextScope &operator=(const SourceTextScope &) = delete;

private:
  const char *previous_name_;
  const char *previous_text_;
};

// Detours PushFileToParserStack, which is what lets ParseSource hand the parser a
// source text instead of a file. RAII, like every other *System - construct and
// destroy inside a Detours transaction. Nothing else in this header needs it: the
// rest resolves its offsets lazily per call.
class GlsSystem {
public:
  GlsSystem();
  ~GlsSystem();
};

// Converts every parsed object into live game data (roles are registered in the
// roles hash, ammo fills the ammo tables, maps load level geometry, ...) and
// releases the objects. The list must not be used afterwards.
void ConvertParsedObjects(ParsedObjectList *list);

// Releases the parsed objects and the list without converting them.
void FreeParsedObjectList(ParsedObjectList *list);

// Converts a single parsed role and returns the game Role (registered in the
// roles hash, id assigned from the global entity counter). Returns the cached
// Role if this parsed object was already converted. Null if incomplete.
Role *ToRole(ParsedThing *thing);

// --- Programmatic object construction and registration ----------------------
//
// Builds the same ParsedThing objects the script parser produces, using the
// game's own constructors and validation, so game objects can be defined and
// registered directly from C++ without a .gls file:
//
//   auto *chr = gls::Create(gls::SectionType::Character);
//   gls::Set(*chr, gls::FieldId::WalkingSpeed, 1.0);
//   gls::Set(*chr, gls::FieldId::TurningSpeed, 0.5);
//   gls::Set(*chr, gls::FieldId::Strength, 20.0);
//   ... (see gls_system_notes.md for each type's required fields)
//
//   auto *role = gls::Create(gls::SectionType::Role);
//   gls::Set(*role, gls::FieldId::Identifier, "my_robot");
//   gls::Set(*role, gls::FieldId::Ai, 0 /* bot */);
//   gls::Set(*role, gls::FieldId::Character, chr);
//   gls::Release(chr); // the role now holds its own reference
//   gls::SetNone(*role, gls::FieldId::Shape);
//
//   if (Role *registered = gls::ToRole(role)) { /* in the roles hash now */ }
//   gls::Release(role);

// Allocates a fresh parsed object of the given section type via the game's own
// section constructor; every field starts at its section default. Returns null
// for SectionType::Unknown. Refcounted - Release when done.
ParsedThing *Create(SectionType type);

// --- Reflection --------------------------------------------------------------
//
// Every section constructor writes the schema it accepts into the object itself:
// field_types says which of the 137 global ids this section uses and how they are
// typed, field_names holds the GLS keyword for each ("walking speed"),
// field_satisfied starts true for a field that has a default and false for one
// that is required, and min_values/max_values hold the bounds CheckValue enforces
// (for String and Custom, min_values[id].boolean is instead "may be none").
//
// So the game is its own schema, and nothing here is a hand-maintained table that
// can drift from gls_system_notes.md. SectionFields builds each list once, by
// constructing a throwaway instance and reading what its constructor declared.

// The GLS section keyword - "role", "camera track", ... - and its inverse
// (case-insensitive, and both "camera track" and "camera_track" resolve).
const char *SectionTypeName(SectionType type);
SectionType SectionTypeFromName(const char *name);

// Every section type in declaration order, terminated by SectionType::Unknown.
// SectionType::Directory is included but converting one changes a game directory.
const SectionType *AllSectionTypes();

struct FieldInfo {
  FieldId id;
  const char *name; // the GLS keyword, e.g. "walking speed"
  FieldType type;
  // False means the field must be assigned before the object converts: IsValid
  // fails without it, and the object is only usable as an `abstract` parent.
  bool optional;
  // String and Custom only: whether `none` is an accepted value.
  bool none_ok;
  // Numeric bounds as CheckValue compares them. Integer and Boolean use the
  // integral pair, Float the floating one; both are filled for either.
  int32_t min_integer, max_integer;
  double min_float, max_float;
};

// The fields `type` accepts, ordered by id. Empty for SectionType::Unknown.
const std::vector<FieldInfo> &SectionFields(SectionType type);
// The one field of `type` whose keyword matches, or null. Matching ignores case
// and treats '_' and ' ' as the same character, so "walking_speed" finds
// "walking speed".
const FieldInfo *FindField(SectionType type, const char *name);

// --- Keyword probing -----------------------------------------------------------
//
// Recovers the integer behind a GLS enum keyword (`ai bot`, `type explode`,
// `action on death must drop`). Those keywords are compiled into the flex DFA
// rather than stored as strings, so they cannot be read out of the binary - but
// the parser will happily tell us, if asked in the right shape.
//
// ProbeKeywords builds a throwaway script in memory - one parse per keyword, each
// carrying just the field under test:
//
//     ammo info GkPlusProbe0 { ammo type flares }
//     ammo info GkPlusProbe1 { ammo type plasma bolts }
//
// parses it, and reads `parsed_values[field]` back out of each object. CheckValue
// stores a value whether or not the rest of the section is complete, so a
// one-field section is enough; nothing is converted, so no game state is touched.
//
// `out` receives one entry per input keyword, in order.
//
// **It stops at the first keyword the parser refuses**, leaving that entry and
// every later one at `std::nullopt` - so a `nullopt` means "rejected, or never
// tested", not "rejected". That is not caution: a syntax error **poisons the
// parser for the rest of the process**. LoadGLS resets its error counter,
// ParsedObjList and the symbol tables on entry, but not the file stack, and no
// later parse recovers - even a verbatim copy of a shipped section fails. So
// feed this only keywords harvested from scripts the game actually parses, and
// treat a stop as "fix the input and run again".
//
// LoadGLS uses destructive global parser state: this must NOT be called while the
// game is loading a level. From the menu, or from a console command, it is fine.
bool ProbeKeywords(SectionType section, FieldId field,
                   const std::vector<std::string> &keywords,
                   std::vector<std::optional<int32_t>> *out);

// Parses `source` as a .gls and reports how many objects reached
// ParsedObjList - the bisection tool for working out why a generated script does
// not parse. -1 means LoadGLS returned nothing at all (a syntax error, or every
// section demoted to abstract); 0 or more is the object count.
//
// Nothing is converted, so this is safe to call repeatedly. Same caveat as
// ProbeKeywords: destructive global parser state, never during a level load.
int TryParse(const char *source);

// Clears the converted-object cache a role ParsedThing keeps at +0x1b60, so the
// next ToRole builds a fresh Role instead of handing back the previous one.
//
// This is what makes a parsed role reusable across level loads. ToRole is gated
// on that dword being null and stores the Role there on success, while
// DestroyRoles frees every Role between levels - so a role object kept in a
// script would otherwise resolve to a pointer into a freed pool page on the
// second load. Harmless for the other section types, which have no cache.
void ResetConversionCache(ParsedThing *thing);

// Drops one reference; destroys the object (via its virtual dtor, on the
// game's heap) when the count reaches zero. Converted game objects survive.
void Release(ParsedThing *thing);

// Copies every field value from parent into thing (the script `child : parent`
// inheritance). Both must be the same section type.
bool InheritFrom(ParsedThing &thing, const ParsedThing &parent);

// Field setters going through the game's own validation (CheckValue): range
// checks, `none`-allowed checks and sub-object type checks apply, and rejected
// assignments are reported to the game console. Unlike script parsing,
// re-assigning an already-set field is allowed. Values use the same units as
// .gls files (angles in degrees, times in seconds, ...). Returns false if the
// field has a different type or the value was rejected.
bool Set(ParsedThing &thing, FieldId id, bool value);            // Boolean
bool Set(ParsedThing &thing, FieldId id, int32_t value);         // Integer
bool Set(ParsedThing &thing, FieldId id, double value);          // Float
bool Set(ParsedThing &thing, FieldId id, const char *value);     // String
bool Set(ParsedThing &thing, FieldId id, ParsedThing *object);   // Custom;
// the parent takes its own reference - Release yours when done building.

// Explicitly assigns `none` to a String or Custom field (only valid where the
// section allows it - see the "none ok" column in gls_system_notes.md).
bool SetNone(ParsedThing &thing, FieldId id);

bool IsValid(const ParsedThing &thing);     // all required fields assigned
bool IsValidDeep(const ParsedThing &thing); // ... recursively in sub-objects

// Converts the object into live game data via its toGameObject vtable slot:
// roles enter the roles hash (returned as Role*), ammo / ammo info fill the
// global ammo tables, camera tracks and directory entries register themselves.
// Shape/hierarchy/light/pgenerator/projectile/character/destructibility
// objects are consumed through their parent role instead (their slot is a
// no-op). Refuses maps (converting one loads level geometry) and objects that
// fail IsValidDeep; returns null in those cases and for void converters.
void *RegisterGameObject(ParsedThing &thing);
} // namespace gk::gls
