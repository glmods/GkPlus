#include "GLS.h"

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
