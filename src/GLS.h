#pragma once

#include "Core.h"
#include "List.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

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
