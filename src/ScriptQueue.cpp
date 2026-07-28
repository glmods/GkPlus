#include "ScriptQueue.h"

#include "Actors.h"
#include "Core.h"
#include "DetourUtils.h"
#include "Encoding.h"
#include "Json.h"
#include "Math.h"
#include "Memory.h"
#include "Menu.h"
#include "Misc.h"
#include "Roles.h"
#include "Triggers.h"
#include "Vulnerability.h"

#include <cstdio>
#include <cstring>
#include <string>

// DetourUtils.h is included for the member-pointer overloads, which slot 66 needs.
// Its gk::DetourAttach then hides Detours' own template for plain function
// pointers, so every plain hook below qualifies the call as ::DetourAttach.

namespace gk {
namespace {

// Trampolines. Calling through one of these runs the original, which is what
// makes the queue hook skippable (QueueScriptMessage), the window hooks thin, and
// the writer hooks able to hand on a value they have rewritten.
FastCall<void, const char *> QueueScriptExecution;
StdCall<void> RunQueuedScript;
FastCall<void, unsigned *> ApplyUpdateMessage;
FastCall<bool, const char *> ExecuteCommandFile;
// Named after the Ghidra DB rather than after gk::RegisterTriggers, which is the
// wrapper in Triggers.h - a trampoline of the same name would make every use of
// either ambiguous inside namespace gk.
FastCall<void, TriggerKind, Vec3 *, long long, TriggerList, const unsigned char *,
         int>
    AddTriggerToGlobalList;
// Both converters are __thiscall on the ParsedThing with nothing but `this` in
// ECX, which is ABI-identical to a one-argument __fastcall.
FastCall<Role *, void *> ToRole;
FastCall<ReplaceDestructibility *, void *> ToReplaceDestructibility;
// The game's own strdup @ 0x0044e1a0, so a rewritten field is allocated exactly
// where the converter allocated the one it replaces.
FastCall<char *, const char *> GameStrdup;
// The two console commands whose script name never reaches a field, and the
// respawn that builds one on the stack. All three are *replaced* rather than
// wrapped, except CommandVulnerability, whose trampoline the sweep calls.
FastCall<void, int, char *> CommandBatchAndBroadcast;
FastCall<void, int, char *> CommandVulnerability;
FastCall<void, char *, int, Vec3, Vec4> MultiplayerRespawnRole;

ScriptMessageHandler Handler = nullptr;

// --- provenance of a script-name write -------------------------------------------

bool EncodedPayload = false;

void Note(const char *what, const char *detail) {
  char buf[512];
  std::snprintf(buf, sizeof(buf), "gkplus script queue: %s: %.400s", what,
                detail ? detail : "(null)");
  DebugWrite(buf);
}

// --- the payload window --------------------------------------------------------
//
// Both consumers pop the payload themselves and hand it straight to
// ExecuteCommandFile, so rather than reimplementing either - which would mean
// duplicating MsgQueue_Pop, the GL_Scripts directory dance and the pool_free of
// the popped buffer - each is wrapped in a window that marks *provenance*, and
// the ExecuteCommandFile hook interprets whatever arrives inside one.
//
// That the window is unambiguous is a measured fact, not an assumption.
// RunQueuedScript is 13 instructions with exactly one ExecuteCommandFile call,
// and ApplyUpdateMessage contains exactly one (0x004ff971), reachable only from
// `case 0x67`, with none of its 164 direct callees reaching it transitively
// (directplay_protocol_notes.md §8.11). ExecuteCommandFile has six callers in
// all; the other four - LoadLevel's level .gcs, the console's own BATCH, and the
// two briefing-screen ones - are outside both windows and pass through untouched.
//
// The window is **one-shot**: the first interception disarms it, so a payload
// that goes on to run a batch file with `#EXECUTE IMMEDIATELY` cannot have that
// file re-read as a payload. Both consumers are main-thread.
bool PayloadPending = false;

// Saves and restores rather than clearing, so a window opened inside another -
// which the tick's ordering makes unreachable today, since it calls
// RunQueuedScript and ClientReceivePump one after the other - would leave the
// outer one armed for the payload it has not popped yet.
struct PayloadWindow {
  bool outer;
  PayloadWindow() : outer(PayloadPending) { PayloadPending = true; }
  ~PayloadWindow() { PayloadPending = outer; }
};

// --- the writer hooks ------------------------------------------------------------
//
// Each takes the bare name the engine is about to store and stores a JSON string
// instead, so a script-name field - and every savegame that serialises one - holds
// a document from the moment it is set. None of them guesses: a game-side write is
// always a bare name, and GkPlus's own writes come in inside an
// EncodedPayloadScope.
//
// `ShouldEncode` is the whole of that decision, and the empty-value case matters:
// an empty string is a legitimate "no script" for some callers, and quoting it
// into `""` would turn it into a payload that opens a file called nothing.
bool ShouldEncode(const char *value) {
  return value && *value && !PayloadIsEncoded();
}

// A game string on its way into a payload: ANSI out of the engine, UTF-8 into the
// JSON. Every writer below goes through this rather than quoting the bytes it was
// handed - see Encoding.h for why passing them through only appears to work.
std::string QuoteGameText(const char *ansi) {
  return json::Quote(Utf8FromGameText(ansi).c_str());
}

// RegisterTriggers @ 0x0043e240 - the only writer of TriggerData::script_name, and
// it covers all 23 game-side registrations: 21 branches of CommandAddTrigger (the
// console `ADD TRIGGER`) plus one each in LoadLevel and Frag. It strdups the
// string, so a temporary is enough.
void __fastcall HookedRegisterTriggers(TriggerKind kind, Vec3 *coords,
                                       long long value, TriggerList targets,
                                       const unsigned char *script, int team) {
  const char *name = reinterpret_cast<const char *>(script);
  if (!ShouldEncode(name)) {
    AddTriggerToGlobalList(kind, coords, value, targets, script, team);
    return;
  }
  std::string quoted = QuoteGameText(name);
  AddTriggerToGlobalList(kind, coords, value, targets,
                         reinterpret_cast<const unsigned char *>(quoted.c_str()),
                         team);
}

// PickupActor::Associate @ 0x005469f0, Actor vtable slot 66 - the only writer of
// associated_script, since Actor::Associate @ 0x0054e640 is a `RET 0x8` stub that
// discards its arguments. It frees the old value and strdups the new one.
//
// A member function is what models __thiscall here: `this` in ECX with the rest on
// the stack, which a __fastcall hook would get wrong by taking `script` in EDX.
// The struct exists only for that calling convention - it is never instantiated,
// and `this` is really a PickupActor.
struct AssociateAbi {
  void HookedAssociate(char *script, bool one_shot);
};
void (AssociateAbi::*Associate)(char *, bool) = nullptr;

void AssociateAbi::HookedAssociate(char *script, bool one_shot) {
  if (!ShouldEncode(script)) {
    (this->*Associate)(script, one_shot);
    return;
  }
  std::string quoted = QuoteGameText(script);
  (this->*Associate)(const_cast<char *>(quoted.c_str()), one_shot);
}

// Replaces an owned pool string with `text`, allocated the way the converter
// allocated the old one. Null on allocation failure, in which case the caller
// leaves the field alone rather than losing it.
char *RepoolString(const char *text) { return GameStrdup(text); }

// ToRole @ 0x0047cc20 - GLS `interface beam script` (field 0x4c) lands in
// Role::interface_beam_script, and AddInterfaceBeamVulnerability @ 0x00510fe0
// later copies the *pointer* into Vulnerability::script, so quoting it here covers
// the vulnerability path too.
//
// The cache check is load-bearing. ToRole early-returns the Role it cached at
// parsed+0x1b60 (the test is its 13th instruction, 0x0047cc50), and nested
// conversions do call it again - ToFragData builds `role` and `replace_role`
// through it - so without this the field would be quoted once per call.
// 0x1b60 is sizeof(ParsedThingBase), i.e. the first field past the base.
Role *__fastcall HookedToRole(void *parsed) {
  constexpr size_t RoleCacheOffset = 0x1b60;
  bool fresh = parsed && *reinterpret_cast<void **>(
                             static_cast<char *>(parsed) + RoleCacheOffset) == nullptr;
  Role *role = ToRole(parsed);
  if (!role || !fresh || !ShouldEncode(role->interface_beam_script)) {
    return role;
  }
  std::string quoted = QuoteGameText(role->interface_beam_script);
  if (char *fresh_string = RepoolString(quoted.c_str())) {
    // Safe to release: Roles.h records this field as leaked - RoleDtor does not
    // free it - so nothing else will touch the old allocation.
    pool_free(role->interface_beam_script);
    role->interface_beam_script = fresh_string;
  }
  return role;
}

// ToReplaceDestructibility @ 0x0047eaa0 - GLS field 0x00, whose keyword is `name`
// but whose only reader is Frag queueing it. No cache to guard: it pool_allocs a
// fresh record on every call, so two roles sharing one parsed section get one
// each.
ReplaceDestructibility *__fastcall HookedToReplaceDestructibility(void *parsed) {
  ReplaceDestructibility *record = ToReplaceDestructibility(parsed);
  if (!record || !ShouldEncode(record->script.get())) {
    return record;
  }
  std::string quoted = QuoteGameText(record->script.get());
  if (char *fresh_string = RepoolString(quoted.c_str())) {
    record->script.reset(fresh_string); // frees the old through pool_free
  }
  return record;
}

// --- the sites that queue without storing ------------------------------------------
//
// These build a .gcs name and hand it straight to QueueScriptExecution, so there
// is no field to convert - the only way to keep a bare name from existing at all
// is to replace the body. The two below are small enough to reproduce call for
// call from the decompilation, with the name encoded at the site and (where the
// original leaked it) the allocation dropped.
//
// `OnFlagCaptured` is the one that is not: its 760 bytes also drive two
// BroadcastToPlayers payloads, five TeamSlots writes, a vtable getter/setter pair
// and a special case for the role named "Hark". Duplicating its team-to-script
// switch a few instructions earlier would only add a way to pick the wrong file,
// so its name is encoded where every other stray one is - by ScriptQueuePayload,
// at the queue hook.
//
// Both run on the **executor thread**, so nothing here may touch the script host.

// The bits of the game these three need, resolved per call like any other native
// wrapper - none of them is hooked, so there is no trampoline to keep.
int LevelLoadReason() {
  int *p;
  GetObjectAtOffset(p, 0x007b9cf0);
  return *p;
}

void EnterScriptsDirectory() {
  FastCall<unsigned, int> fn;
  GetObjectAtOffset(fn, 0x00466b80);
  fn(0); // GL_Scripts
}

void LeaveScriptsDirectory() {
  StdCall<void> fn;
  GetObjectAtOffset(fn, 0x00466b90);
  fn();
}

void ConsolePrint(const char *text) {
  FastCall<void, const char *> fn;
  GetObjectAtOffset(fn, 0x004d4b50);
  fn(text);
}

// Console `BATCHANDBROADCAST` @ 0x00448400 - "a bit like BATCH but it tells
// clients to batch it as well". Five calls in the original, all reproduced; the
// guard included, since `LevelLoadReason == 3` is what stops a savegame restore
// re-running one.
void __fastcall HookedCommandBatchAndBroadcast(int, char *) {
  if (LevelLoadReason() == 3 || !IsSimulationRunning()) {
    return;
  }
  char *word_buf;
  GetObjectAtOffset(word_buf, 0x006af5f8); // g_ConsoleWordBuf
  FastCall<char *, char *> copy_remaining_args;
  GetObjectAtOffset(copy_remaining_args, 0x004d6d00);
  copy_remaining_args(word_buf);

  EnterScriptsDirectory();
  std::string payload = QuoteGameText(word_buf);
  QueueScriptExecution(payload.c_str()); // the trampoline: already encoded
  LeaveScriptsDirectory();
}

// MULTIPLAYER RESPAWN @ 0x0050c8b0, from EvaluateTriggers. Reproduced from the
// decompilation - it is small enough - with two things the original got wrong left
// out: it malloc'd the 15-byte name and never freed it, and `FUN_00511600` is
// __thiscall on RespawnRoleList @ 0x007b9d98, whose ECX `this` the decompiler
// hides, so the C reads as a one-argument call.
//
// That list is not incidental: it is how the queued script finds the actor. The
// .gcs equips whatever is at its head with GIVE [AND EQUIP] ROLE ID and pops it
// with NEXT RESPAWN ID, so the append has to stay exactly where the original put
// it - after the spawn, before the queue.
void __fastcall HookedMultiplayerRespawnRole(char *role_name, int team, Vec3 pos,
                                             Vec4 orient) {
  Role *role = GetRoleByName(role_name);
  if (!role) {
    ConsolePrint(ResourceString(0x2b0d)); // GL_ERROR_UNIDENTIFIED_ROLE
    return;
  }
  if (IsSimulationRunning()) {
    int spawned = SpawnRole(team, role, &pos, &orient, -1);
    void *respawn_list;
    GetObjectAtOffset(respawn_list, 0x007b9d98);
    ThisCall<void, void *, int *> add_entry;
    GetObjectAtOffset(add_entry, 0x00511600);
    add_entry(respawn_list, &spawned);
  }
  // GameMode 5 is CaptureTheFlag; everything else takes the RTP script. Both are
  // our own ASCII literals, so they need no transcoding - only encoding.
  std::string payload =
      json::Quote(GetGameMode() == 5 ? "CTFRespawn.gcs" : "RTPRespawn.gcs");
  QueueScriptExecution(payload.c_str());
}

// --- the vulnerability sweep -------------------------------------------------------

// CommandVulnerability @ 0x0044a600 sets Vulnerability::script from the console
// line, in 1369 bytes that also parse actor and role names and fan one
// Vulnerability* out to every live actor of a role. Rather than reproduce that,
// the command is wrapped and the result swept: any vulnerability left holding
// something that is not a document gets encoded.
//
// A sweep rather than a before/after diff on purpose. The diff would have to
// assume the pool never hands back the address it just freed - which
// CommandVulnerability does do, since it frees the previous script before
// assigning - whereas "is it encoded yet?" is true or false regardless of how the
// value got there. Every other writer encodes, so the only things this can find
// are this command's own work and a vulnerability restored from an older save.
//
// The residual ambiguity is a console-set script file named exactly `123` or
// `null`, which would already be a document and is left alone.
void EncodeVulnerability(Vulnerability *vuln) {
  if (!vuln || !vuln->script || !*vuln->script.get() ||
      json::Classify(vuln->script.get()) != json::Kind::Invalid) {
    return;
  }
  std::string quoted = QuoteGameText(vuln->script.get());
  if (char *fresh = GameStrdup(quoted.c_str())) {
    vuln->script.reset(fresh); // frees the old through pool_free
  }
}

void __fastcall HookedCommandVulnerability(int length, char *args) {
  CommandVulnerability(length, args);

  // Both lists, because a role-scoped vulnerability lives in the role's list and
  // is aliased into every actor of that role, while an actor-scoped one is only
  // in the actor's. The same pointer showing up twice is harmless - encoding is
  // idempotent, since the second visit finds a document.
  if (Roles *roles = GetRolesTable()) {
    for (Role *role : *roles) {
      for (Vulnerability *vuln : role->vulnerabilities) {
        EncodeVulnerability(vuln);
      }
    }
  }
  if (Actors *actors = GetActorsTable()) {
    for (Actor *actor : *actors) {
      for (Vulnerability *vuln : actor->vulnerabilities) {
        EncodeVulnerability(vuln);
      }
    }
  }
}

// --- the queue hooks -------------------------------------------------------------

// Guards the invariant for what the writers do not cover: the three sites that
// build a name and queue it without storing it, CommandVulnerability's field, an
// old savegame, and a peer without GkPlus. See ScriptQueue.h.
//
// Runs on the **executor thread** for the six simulation-side callers and on the
// main thread for the console's BATCH, so it stays pure string work: no console
// printing, no QuickJS, nothing that takes a game lock. DebugWrite is
// OutputDebugString, which is safe on either.
void __fastcall HookedQueueScriptExecution(const char *payload) {
  if (!payload) {
    QueueScriptExecution(payload); // as vanilla: strlen_plus1 will fault
    return;
  }
  std::string document = ScriptQueuePayload(payload);
  QueueScriptExecution(document.c_str());
}

// Host consumer: pops one payload per frame, from the in-game tick.
void __stdcall HookedRunQueuedScript() {
  PayloadWindow window;
  RunQueuedScript();
}

// Joiner consumer: `case 0x67` sits inside this, gated on !IsExecutorRunning()
// so a host - which already ran the payload off its own queue - skips it. Every
// other update id passes through the window untouched, since none of them
// reaches ExecuteCommandFile.
void __fastcall HookedApplyUpdateMessage(unsigned *message) {
  PayloadWindow window;
  ApplyUpdateMessage(message);
}

// The one place a payload is consumed. Outside a window this is an ordinary
// ExecuteCommandFile - the level .gcs, a console BATCH - and is forwarded
// unexamined.
bool __fastcall HookedExecuteCommandFile(const char *file) {
  if (!PayloadPending || !file) {
    return ExecuteCommandFile(file);
  }
  PayloadPending = false;

  std::string name;
  switch (json::Classify(file, &name)) {
  case json::Kind::String: {
    // The legacy meaning, spelled as JSON. The engine has already set the
    // current directory to Scripts, so this is the call it was going to make -
    // back through the encoding boundary first, because fopen reads a char *
    // through the active codepage and the payload held UTF-8.
    std::string ansi = GameTextFromUtf8(name.c_str());
    return ExecuteCommandFile(ansi.c_str());
  }
  case json::Kind::Invalid: {
    // Nothing local can produce this: the queue hook above cannot emit a
    // non-document, and update 0x67 is the only message that reaches here. So it
    // came off the wire from a vanilla peer. Run it as it stands, since that is
    // what it means and what it needs.
    static bool reported = false;
    if (!reported) {
      reported = true;
      Note("a peer is not running GkPlus - its payloads are bare file names, "
           "which will be run as such for the rest of the session, starting with",
           file);
    }
    return ExecuteCommandFile(file);
  }
  default:
    if (!Handler || !Handler(file)) {
      // Not on the console: a message arrives once per trigger firing on every
      // machine, and a level that ignores one would flood it.
      Note("message went undelivered", file);
    }
    // The return says "the file opened", and none of the engine's six callers
    // reads it. Consumed is the honest answer for a payload that was never a
    // file.
    return true;
  }
}

} // namespace

EncodedPayloadScope::EncodedPayloadScope() : outer_(EncodedPayload) {
  EncodedPayload = true;
}

EncodedPayloadScope::~EncodedPayloadScope() { EncodedPayload = outer_; }

bool PayloadIsEncoded() { return EncodedPayload; }

std::string ScriptQueuePayload(const char *stored, bool *repaired) {
  if (repaired) {
    *repaired = false;
  }
  if (stored && json::Classify(stored) != json::Kind::Invalid) {
    return stored;
  }
  if (repaired) {
    *repaired = true;
  }
  // Quote accepts any byte string, so this branch cannot fail - which is what
  // makes the return value unconditionally a document.
  return QuoteGameText(stored);
}

void SetScriptMessageHandler(ScriptMessageHandler handler) {
  Handler = handler;
}

bool QueueScriptMessage(const char *json) {
  if (!json || json::Classify(json) == json::Kind::Invalid) {
    return false;
  }
  if (!QueueScriptExecution) {
    // Deliberately not falling back to the raw address: after DetourAttach that
    // entry point is the hook, which would re-encode a document that is already
    // one.
    Note("not installed, message dropped", json);
    return false;
  }
  QueueScriptExecution(json);
  return true;
}

ScriptQueueSystem::ScriptQueueSystem() {
  GetObjectAtOffset(QueueScriptExecution, 0x00505080);
  GetObjectAtOffset(RunQueuedScript, 0x00505310);
  GetObjectAtOffset(ApplyUpdateMessage, 0x004fde70);
  GetObjectAtOffset(ExecuteCommandFile, 0x0043f250);
  GetObjectAtOffset(AddTriggerToGlobalList, 0x0043e240);
  GetObjectAtOffset(ToRole, 0x0047cc20);
  GetObjectAtOffset(ToReplaceDestructibility, 0x0047eaa0);
  GetObjectAtOffset(Associate, 0x005469f0);
  GetObjectAtOffset(GameStrdup, 0x0044e1a0);
  GetObjectAtOffset(CommandBatchAndBroadcast, 0x00448400);
  GetObjectAtOffset(CommandVulnerability, 0x0044a600);
  GetObjectAtOffset(MultiplayerRespawnRole, 0x0050c8b0);

  ::DetourAttach(&QueueScriptExecution, HookedQueueScriptExecution);
  ::DetourAttach(&RunQueuedScript, HookedRunQueuedScript);
  ::DetourAttach(&ApplyUpdateMessage, HookedApplyUpdateMessage);
  ::DetourAttach(&ExecuteCommandFile, HookedExecuteCommandFile);
  ::DetourAttach(&AddTriggerToGlobalList, HookedRegisterTriggers);
  ::DetourAttach(&ToRole, HookedToRole);
  ::DetourAttach(&ToReplaceDestructibility, HookedToReplaceDestructibility);
  ::DetourAttach(&CommandBatchAndBroadcast, HookedCommandBatchAndBroadcast);
  ::DetourAttach(&CommandVulnerability, HookedCommandVulnerability);
  ::DetourAttach(&MultiplayerRespawnRole, HookedMultiplayerRespawnRole);
  DetourAttach(&Associate, &AssociateAbi::HookedAssociate);
}

ScriptQueueSystem::~ScriptQueueSystem() {
  ::DetourDetach(&QueueScriptExecution, HookedQueueScriptExecution);
  ::DetourDetach(&RunQueuedScript, HookedRunQueuedScript);
  ::DetourDetach(&ApplyUpdateMessage, HookedApplyUpdateMessage);
  ::DetourDetach(&ExecuteCommandFile, HookedExecuteCommandFile);
  ::DetourDetach(&AddTriggerToGlobalList, HookedRegisterTriggers);
  ::DetourDetach(&ToRole, HookedToRole);
  ::DetourDetach(&ToReplaceDestructibility, HookedToReplaceDestructibility);
  ::DetourDetach(&CommandBatchAndBroadcast, HookedCommandBatchAndBroadcast);
  ::DetourDetach(&CommandVulnerability, HookedCommandVulnerability);
  ::DetourDetach(&MultiplayerRespawnRole, HookedMultiplayerRespawnRole);
  DetourDetach(&Associate, &AssociateAbi::HookedAssociate);
}
} // namespace gk
