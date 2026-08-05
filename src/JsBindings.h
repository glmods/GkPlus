#pragma once

#include "Math.h"

#include <quickjs.h>

#include <cstdint>
#include <string>
#include <vector>

// Internal to the "gk" module - included only by src/Js*.cpp.
//
// Two QuickJS rules govern everything in this layer, both verified against
// quickjs-ng 0.15.1's own source rather than assumed:
//
//   * A JS_CGETSET_DEF entry reaching JS_SetModuleExportList hits that switch's
//     `default: abort()` (quickjs.c:40006) - the process dies with no
//     diagnostic. JS_SetPropertyFunctionList is the one instantiation path that
//     honours JS_DEF_CGETSET. So this layer never calls JS_SetModuleExportList
//     at all; every namespace is built object-first and handed to
//     JS_SetModuleExport as a finished value.
//   * A C module's named exports are values set once at instantiation, never
//     live bindings. `import { position } from "gk"` could only ever be a
//     snapshot; exporting objects and putting the accessors on them is what
//     makes the state live.

namespace gk {
struct Actor;
struct Role;
} // namespace gk

namespace gk::js {

// --- Vec3/Vec4 marshalling ---------------------------------------------------

JSValue NewVec3(JSContext *ctx, const Vec3 &v);
JSValue NewVec4(JSContext *ctx, const Vec4 &v);

// Read {x, y, z[, w]} off any object. Missing components keep whatever `out`
// already holds, so callers can pre-seed a default. False means an exception is
// pending.
bool ToVec3(JSContext *ctx, JSValueConst v, Vec3 *out);
bool ToVec4(JSContext *ctx, JSValueConst v, Vec4 *out);

// --- option-object readers ---------------------------------------------------
//
// Each leaves *out untouched when the property is absent, undefined or null, so
// the caller's initialiser is the default. False means an exception is pending.

bool GetInt32Prop(JSContext *ctx, JSValueConst obj, const char *name,
                  int32_t *out);
bool GetInt64Prop(JSContext *ctx, JSValueConst obj, const char *name,
                  int64_t *out);
bool GetFloatProp(JSContext *ctx, JSValueConst obj, const char *name,
                  float *out);
// Reads an array of up to `count` Vec3s. Entries past the array's end are left
// alone - RegisterTriggers reads all four unconditionally, so the caller must
// zero-initialise.
bool GetVec3ArrayProp(JSContext *ctx, JSValueConst obj, const char *name,
                      Vec3 *out, int count);

// --- script-queue payloads ---------------------------------------------------

// What a script writes into one of the engine's script-name fields - a trigger's
// script, a pickup's associated script, an interface beam's, a destructibility's -
// now that those fields carry JSON (see ScriptQueue.h). The result is always a
// complete JSON document, and the value's type picks which:
//
//   * a **string** is a .gcs file name, encoded as a JSON string. That is exactly
//     what the engine-side writer hooks do to a bare name, so a field written from
//     a script and one written from a .gls end up looking the same.
//   * **anything else** is a message, JSON-encoded, and arrives at a script
//     level's `message_received` instead of being opened as a file.
//
// Filling one of GkPlus's own structs with the result needs nothing further
// (make.role). Passing it to an engine *setter* does: wrap that call in an
// EncodedPayloadScope, or the writer hook will quote the document again.
//
// undefined and null leave *out alone, so a caller's default survives. False
// means an exception is pending - including for a value JSON.stringify refuses,
// which would otherwise store the word "undefined" as a file name.
bool ToScriptPayload(JSContext *ctx, JSValueConst v, std::string *out);

// The same, read off a property. Absent, undefined and null all leave *out
// alone.
bool GetScriptPayloadProp(JSContext *ctx, JSValueConst obj, const char *name,
                          std::string *out);

// --- namespace objects -------------------------------------------------------

// A plain object carrying `props`. This is the only way accessors get installed
// anywhere in this layer.
JSValue NewNamespace(JSContext *ctx, const JSCFunctionListEntry *props, int len);

// --- indexable collections ---------------------------------------------------

// Drives the exotic-property behaviour that makes `actors`, `roles` and `tokens`
// behave like keyed collections: actors[12], actors["tbaa"], "tbaa" in actors,
// Object.keys(actors), Object.entries(tokens), for (const a of actors).
//
// Both lookups return JS_UNDEFINED for a miss (not an exception) - a missing key
// must read as "no such own property", not as an error.
struct CollectionOps {
  const char *class_name;
  // Null for a collection with no integer keys (tokens): a wholly-decimal key
  // then falls through to lookup_name, so a token literally called "5" still
  // resolves.
  JSValue (*lookup_id)(JSContext *ctx, int id);
  JSValue (*lookup_name)(JSContext *ctx, const char *name);
  // The enumerable own keys, as strings - decimal ids for actors and roles,
  // names for tokens. This is what Object.keys and the iterator walk.
  void (*collect_keys)(std::vector<std::string> *out);
  unsigned (*count)();
  // Assignment through the indexer: `tokens["score"] = 0`. Return 1 on success,
  // 0 to refuse, -1 with an exception pending. Null makes the collection a
  // read-only view, where an assignment is a no-op in sloppy mode and a
  // TypeError in strict - so always a TypeError from a module.
  //
  // Own properties are resolved before this ever runs (quickjs.c:10137 vs
  // :10203), so `tokens.count = 5` hits the read-only accessor and cannot reach
  // here to create a bogus entry.
  int (*assign)(JSContext *ctx, const char *key, JSValueConst value);
  // Per-namespace extras only (roles.ai_types); `count` and
  // Symbol.iterator are installed for every collection by NewCollection. May be
  // null. All of it is deliberately non-enumerable: an enumerable `count` would
  // show up in Object.keys(actors) beside the ids, the same reason Array's
  // `length` is hidden. Own properties win over the indexer -
  // JS_GetPropertyInternal calls find_own_property before consulting the exotic
  // hook (quickjs.c:8734).
  const JSCFunctionListEntry *props;
  int props_len;
};

// `ops` must outlive the context; pass a pointer to a file-scope constant.
JSValue NewCollection(JSContext *ctx, JSClassID *class_id,
                      const CollectionOps *ops);

// Registers `class_id` in ctx's runtime if it is not already, and installs a
// prototype carrying `proto_props` for this context. A null `proto_props`
// leaves the class with no prototype at all, which is what a collection wants.
bool EnsureClass(JSContext *ctx, JSClassID *class_id, const JSClassDef *def,
                 const JSCFunctionListEntry *proto_props, int proto_len);

// As above, but chains the new prototype to `parent_proto` - this is how the
// Actor subclass prototypes mirror the C++ hierarchy. Unlike the overload
// above, this one always creates a prototype: a class that adds no members of
// its own still needs one to sit in the chain, so a null `proto_props` here
// means "empty", not "none". `parent_proto` is borrowed; pass JS_UNDEFINED to
// leave Object.prototype as the parent.
bool EnsureClass(JSContext *ctx, JSClassID *class_id, const JSClassDef *def,
                 const JSCFunctionListEntry *proto_props, int proto_len,
                 JSValueConst parent_proto);

// --- wrappers ----------------------------------------------------------------
//
// Both hold the raw game pointer. The game frees actors and roles with no
// notification and pool_free recycles the pages (see Memory.h), so a wrapper
// outliving its object reads recycled memory - that is the accepted trade-off,
// and `.valid` is the escape hatch. Never free the game object from JS.

JSValue NewActorWrapper(JSContext *ctx, Actor *actor);
JSValue NewRoleWrapper(JSContext *ctx, Role *role);

// The Role behind a wrapper, or null when `v` is anything else. Does NOT throw on
// a miss - callers use it to accept "a Role, or nothing", so it has to be a test.
Role *RoleFromWrapper(JSValueConst v);

// --- the namespace builders ---------------------------------------------------

JSValue NewCameraNamespace(JSContext *ctx);
JSValue NewConsoleNamespace(JSContext *ctx);
// Game-wide state that belongs to no other namespace: difficulty, the cheat and
// render flags, the selection, the session toggles.
JSValue NewGameNamespace(JSContext *ctx);
// The level's atmosphere - sun, ambient light and fog. Separate from `game`
// because it is per-level state rather than per-session, and it goes away with
// the level: every `fog` accessor reports nothing while FogSystem is null.
JSValue NewWorldNamespace(JSContext *ctx);

// The broadcast-free console commands, as typed namespaces (src/JsCommands.cpp).
// Unlike everything above, these format and dispatch the game's own command
// rather than calling a native: every one of those handlers *is* an argument
// parser, so running it is faithful by construction. See the header comment
// there for why the split falls where it does.
JSValue NewFxNamespace(JSContext *ctx);
JSValue NewLightNamespace(JSContext *ctx);
JSValue NewObjectivesNamespace(JSContext *ctx);
JSValue NewMusicNamespace(JSContext *ctx);
JSValue NewScreenNamespace(JSContext *ctx);
JSValue NewUnitsNamespace(JSContext *ctx);
JSValue NewInventoryNamespace(JSContext *ctx);
JSValue NewTracksNamespace(JSContext *ctx);
JSValue NewDemoNamespace(JSContext *ctx);
JSValue NewScriptNamespace(JSContext *ctx);

// Formats `name` followed by `argv` into a console command line and runs it.
// Numbers become locale-independent decimals, booleans ON/OFF, {x,y,z} three
// numbers, arrays their elements, and undefined/null stop the line (which is how
// every optional console argument is spelled). Throws on a string containing
// whitespace - the console splits on it and has no quoting - and on a line over
// kConsoleCommandLineMax, which the engine would otherwise copy past the end of
// its 252-byte buffer.
//
// Exported so `camera` and `console` can carry their own command-backed members
// beside their native ones.
JSValue RunConsoleCommand(JSContext *ctx, const char *name, int argc,
                          JSValueConst *argv);
// The same for a command whose name lives in glres<lang>.dll (ids 10000..10014).
JSValue RunLocalizedConsoleCommand(JSContext *ctx, unsigned resource_id,
                                   int argc, JSValueConst *argv);
// For a handler that consumes the rest of the line (`CopyRemainingArgs` rather
// than `ConsoleParseWord`) - LOG, SAY, WAIT FOR, PLAY FMV. Whitespace inside a
// string argument is meaningful there, so it is allowed rather than refused.
JSValue RunConsoleTextCommand(JSContext *ctx, const char *name, int argc,
                              JSValueConst *argv);
JSValue NewActorsNamespace(JSContext *ctx);
JSValue NewRolesNamespace(JSContext *ctx);
JSValue NewTokensNamespace(JSContext *ctx);
JSValue NewTriggersNamespace(JSContext *ctx);
// NewMenusNamespace is in Js.h: it is the one the host builds directly, because
// `menus` is setup_menus' argument rather than a "gk" export.
JSValue NewLevelsNamespace(JSContext *ctx);
JSValue NewGlsNamespace(JSContext *ctx);
JSValue NewMakeNamespace(JSContext *ctx);
// The mod filesystem (src/Vfs.h): which archives are mounted, and what they
// provide. Read-only from script apart from mount(); the interception that makes
// the engine consult it is installed long before any script runs.
JSValue NewModsNamespace(JSContext *ctx);
// The renderer (src/D3D8Capture.h). Currently the Phase 0b measurement surface for the
// Vulkan work - see vulkan_renderer_notes.md. Read-only: the capture layer forwards every
// D3D8 call unchanged, so nothing here can alter what is drawn.
JSValue NewRenderNamespace(JSContext *ctx);
// The game's own text layer (src/Font.h). `text.draw` *queues* - the item is
// rasterized by the per-frame overlay pass and then freed, so a string drawn once
// lasts one frame. For a persistent panel use the ImGui object `draw_gui` is
// handed; this is for text that has to look like the game's own.
JSValue NewTextNamespace(JSContext *ctx);

// --- per-TU callback teardown --------------------------------------------------
//
// Every TU that hands a JSValue to a native registration drops it here, and
// js::ReleaseCallbacks in JsGk.cpp calls the lot. Each also makes its own native
// registrations inert first, because those outlive the context.
void ReleaseMenuCallbacks(JSContext *ctx);
void ReleaseLevelCallbacks(JSContext *ctx);

} // namespace gk::js
