#include "ScriptQueue.h"

#include "Actors.h"
#include "Core.h"
#include "DetourUtils.h"
#include "Encoding.h"
#include "Json.h"
#include "List.h"
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
// Kept only so Detours has a pointer to patch: HookedPumpQueuedConsoleCommand
// replaces the body outright and never calls through this.
StdCall<void> PumpQueuedConsoleCommand;
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

// The vocabulary of payload kinds. `gk::json` owns the envelope's shape and
// nothing else, so this is the only place the three names are spelled.
constexpr const char *KindFile = "file";
constexpr const char *KindCommand = "command";
constexpr const char *KindMessage = "message";

// A .gcs name as a payload: `{"kind":"file","body":"<name>"}`.
std::string FileEnvelope(const char *utf8) {
  return json::Envelope(KindFile, json::Quote(utf8).c_str());
}

// The same for a string on its way out of the engine: ANSI in, UTF-8 into the
// JSON. Every writer below goes through this rather than wrapping the bytes it
// was handed - see Encoding.h for why passing them through only appears to work.
std::string FileEnvelopeFromGameText(const char *ansi) {
  return FileEnvelope(Utf8FromGameText(ansi ? ansi : "").c_str());
}

// Whether a stored value is already one of ours. This is the whole of the
// "has it been encoded yet?" test, and unlike the old one - "does it parse as
// any JSON document at all?" - it has no false positives: see Json.h.
bool IsEnvelope(const char *text) {
  return json::OpenEnvelope(text, nullptr, nullptr);
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
  std::string envelope = FileEnvelopeFromGameText(name);
  AddTriggerToGlobalList(kind, coords, value, targets,
                         reinterpret_cast<const unsigned char *>(envelope.c_str()),
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
  std::string envelope = FileEnvelopeFromGameText(script);
  (this->*Associate)(const_cast<char *>(envelope.c_str()), one_shot);
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
  std::string envelope = FileEnvelopeFromGameText(role->interface_beam_script);
  if (char *fresh_string = RepoolString(envelope.c_str())) {
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
  std::string envelope = FileEnvelopeFromGameText(record->script.get());
  if (char *fresh_string = RepoolString(envelope.c_str())) {
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

// ExecuteCommand @ 0x004d6090 - runs one line immediately, the same entry point
// typing it takes. Resolved here rather than through Console.h so that the
// trampoline named ExecuteCommandFile in this namespace cannot be confused with
// the wrapper of the same name.
void ExecuteConsoleCommand(const char *line) {
  FastCall<void, const char *> fn;
  GetObjectAtOffset(fn, 0x004d6090);
  fn(line);
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
  std::string payload = FileEnvelopeFromGameText(word_buf);
  QueueScriptExecution(payload.c_str()); // the trampoline: already an envelope
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
  // our own ASCII literals, so they need no transcoding - only wrapping.
  std::string payload =
      FileEnvelope(GetGameMode() == 5 ? "CTFRespawn.gcs" : "RTPRespawn.gcs");
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
// This used to have a residual ambiguity - a console-set script file named
// exactly `123` or `null` parses as a document and was left alone - which the
// envelope removes: those are not envelopes, so they get wrapped like any other
// bare name.
void EncodeVulnerability(Vulnerability *vuln) {
  if (!vuln || !vuln->script || !*vuln->script.get() ||
      IsEnvelope(vuln->script.get())) {
    return;
  }
  std::string envelope = FileEnvelopeFromGameText(vuln->script.get());
  if (char *fresh = GameStrdup(envelope.c_str())) {
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

// --- the console command queue -----------------------------------------------------
//
// CommandsToExecute @ 0x007b6aa8 is handled through the same envelope dispatch as
// the script queue, but **GkPlus never writes to it**. Its nodes keep holding the
// plain lines the game put there, and the consumer treats a bare line as
// kind "command" - the branch that already had to exist for savegames written
// before the envelope. So the two queues share one set of meanings and one code
// path, and the console queue keeps byte-for-byte the representation vanilla
// Gunlok gives it.
//
// **That is the second design here, and the first one crashed.** The queue used
// to be swept after every ExecuteCommandFile, rewriting each node's payload into
// a literal `{"kind":"command",...}` with the game's own strdup and pool_free.
// It reproduced as a wild call - EIP on the stack, so WER blamed "module:
// unknown" - roughly two runs in three, from inside the sweep's pool_free. The
// lesson is not "that had a bug" but that a subsystem which rewrites another
// allocator's live objects on a hot path buys very little and risks everything:
// the only thing the rewrite added was that the bytes on the queue *looked* like
// the bytes on the wire.
//
// Two facts make not writing to it free:
//
//   * the consumer must be replaced anyway. PumpQueuedConsoleCommand copies a
//     node's string into ConsoleCommandLine (char[252] @ 0x007b6958) with an
//     unbounded byte loop at 0x004d61f0, and the next global is ConsoleSmallFont
//     @ 0x007b6a54 with nothing in between - so an envelope arriving here from
//     anywhere has to be decoded before it reaches that buffer, and the decode
//     is the same code that handles a bare line.
//   * nothing else has to change. The `#` directives, front-insertion, CLEAR
//     BATCH, NumCommandsToExecute and SaveGame's serialisation of pending lines
//     all keep working because they are simply left alone, and a save written by
//     this build stays readable by vanilla Gunlok.

constexpr size_t ConsoleCommandLineSize = 252;

char *ConsoleCommandLineBuffer() {
  char *p;
  GetObjectAtOffset(p, 0x007b6958);
  return p;
}

// The pump's own save area for the line the player is typing, immediately after
// the buffer it shadows and the same size.
char *ConsoleCommandLineSaveArea() {
  char *p;
  GetObjectAtOffset(p, 0x007b6c68);
  return p;
}

List<char *> *ConsoleQueue() {
  List<char *> *p;
  GetObjectAtOffset(p, 0x007b6aa8);
  return p;
}

void ConsoleExecuteCommandLine(const char *line) {
  FastCall<void, const char *> fn;
  GetObjectAtOffset(fn, 0x004d59e0);
  fn(line);
}

// The flattened-pointer cache the list rebuilds lazily. Every engine site that
// touches the queue drops it, and so must anything that changes a payload in
// place - the cache holds the `char *`s themselves, not their addresses.
void InvalidateConsoleQueueCache() {
  void **cache;
  GetObjectAtOffset(cache, 0x007b6ab0);
  unsigned char *valid;
  GetObjectAtOffset(valid, 0x007b6ab4);
  *valid = 0;
  if (*cache) {
    pool_free(*cache);
    *cache = nullptr;
  }
}

// --- the pump's gates ---------------------------------------------------------
//
// Transcribed from 0x004d6120. Every comparison there is a signed test on the
// high dword followed by an unsigned one on the low (JL/JG/JC), which is exactly
// what a signed 64-bit `<` compiles to - so these read as ordinary comparisons
// without changing a single outcome.

long long ReadScaledClock64(unsigned *conversion) {
  FastCall<long long, unsigned *> fn;
  GetObjectAtOffset(fn, 0x00571bb0);
  return fn(conversion);
}

long long ServerTime64() {
  FastCall<long long> fn;
  GetObjectAtOffset(fn, 0x00505340);
  return fn();
}

template <typename T> T *GlobalAt(unsigned offset) {
  T *p;
  GetObjectAtOffset(p, offset);
  return p;
}

// The REPTXT hint, re-printed while a `wait for` holds the queue. The original
// reaches this as an outlined tail at 0x0056e7f0.
void ReprintRepeatText() {
  if (!*GlobalAt<unsigned char>(0x007ba3cc)) { // RepeatTextActive
    return;
  }
  long long *deadline = GlobalAt<long long>(0x007ba4d0);
  if (ReadScaledClock64(GlobalAt<unsigned>(0x006aaaa0)) < *deadline) {
    return;
  }
  StdCall<void> hide_console;
  GetObjectAtOffset(hide_console, 0x0043fa20);
  hide_console();
  ConsolePrint(GlobalAt<char>(0x007ba3d0)); // RepeatTextBuf
  StdCall<void> arm_deadline;
  GetObjectAtOffset(arm_deadline, 0x0056ea30);
  arm_deadline();
}

// False means "do not pop this frame". A savegame restore bypasses all three,
// which is what stops a restored queue from being held by a restored deadline.
bool ConsoleQueueGatesPass() {
  if (LevelLoadReason() == 3) {
    return true;
  }

  long long *wait = GlobalAt<long long>(0x007b9c88); // WaitDeadline
  if (*wait != 0) {
    // The simulation's clock while one is running, the front end's otherwise -
    // the same choice CommandWait made when it set the deadline.
    long long now = IsSimulationRunning()
                        ? ServerTime64()
                        : ReadScaledClock64(GlobalAt<unsigned>(0x006aaab8));
    if (now < *wait) {
      return false;
    }
    *wait = 0;
  }

  long long *real_wait = GlobalAt<long long>(0x007b9c90); // RealWaitDeadline
  if (*real_wait != 0) {
    long long now = ReadScaledClock64(GlobalAt<unsigned>(0x006aaaa0));
    if (now < *real_wait) {
      // REAL WAIT OR CLICK. Both flags must be set: the first says this wait is
      // cancellable, the second that the key has been pressed. It is TAB, not
      // the mouse the help text claims - NotifyWaitCancelKey @ 0x004d6500 is the
      // only writer and both of its callers test KEY_Tab.
      if (!*GlobalAt<unsigned char>(0x007b9c98) ||
          !*GlobalAt<unsigned char>(0x007b9c99)) {
        return false;
      }
    }
    // One 16-bit store in the original, clearing both flags at once.
    *GlobalAt<unsigned short>(0x007b9c98) = 0;
    *real_wait = 0;
  }

  StdCall<bool> is_wait_for_set;
  GetObjectAtOffset(is_wait_for_set, 0x0056ec90);
  if (is_wait_for_set()) {
    StdCall<bool> check_wait_for;
    GetObjectAtOffset(check_wait_for, 0x0056eab0);
    if (!check_wait_for()) {
      ReprintRepeatText();
      return false;
    }
    StdCall<void> disarm_repeat;
    GetObjectAtOffset(disarm_repeat, 0x0056ea20);
    disarm_repeat();
  }
  return true;
}

// --- popping one -------------------------------------------------------------

// Unlinks the head, destroying the node through vtable slot 0 - the scalar
// deleting destructor, with 1 for "free it too" - exactly as the original does.
// The node is pool memory, so nothing here may use our own operator delete.
void UnlinkFirstConsoleCommand(List<char *> *queue) {
  List_Member_Base<char *> *sentinel = queue->sentinel;
  List_Member_Base<char *> *first = sentinel->next;
  if (first != sentinel) {
    sentinel->next = first->next;
    if (List_Member_Base<char *> *removed = sentinel->next->prev) {
      ThisCall<void, void *, int> destroy = reinterpret_cast<
          ThisCall<void, void *, int>>((*reinterpret_cast<void ***>(removed))[0]);
      destroy(removed, 1);
    }
    sentinel->next->prev = sentinel;
    --queue->n_entries;
  }
  InvalidateConsoleQueueCache();
}

// One payload off the console queue. Uniform with the script queue's consumer -
// same three kinds, same handler - because a restored savegame or a future build
// can put any of them here.
void DispatchConsolePayload(const char *payload) {
  std::string kind;
  std::string body;
  std::string text;

  if (json::OpenEnvelope(payload, &kind, &body)) {
    if (kind == KindMessage) {
      if (!Handler || !Handler(body.c_str())) {
        Note("message went undelivered", payload);
      }
      return;
    }
    if (json::Classify(body.c_str(), &text) != json::Kind::String) {
      Note("payload body is not a string", payload);
      return;
    }
    text = GameTextFromUtf8(text.c_str());
    if (kind == KindFile) {
      ExecuteCommandFile(text.c_str());
      return;
    }
    if (kind != KindCommand) {
      Note("unknown payload kind, dropped", payload);
      return;
    }
  } else {
    // Not an envelope, which is the **normal** case here: GkPlus never writes to
    // this queue, so every node the game queued holds a plain line. It is a
    // console command, which is all it ever was. Old savegames land here too, by
    // the same rule rather than as a special case.
    text = payload ? payload : "";
  }

  if (text.size() >= ConsoleCommandLineSize) {
    // Cannot happen from a queued line - fgets caps those at 249 - but a message
    // from the wire is not bounded by anything, and this buffer's neighbour is a
    // font pointer.
    Note("queued command too long for ConsoleCommandLine, truncated",
         text.c_str());
    text.resize(ConsoleCommandLineSize - 1);
  }

  // Borrow the console's line buffer the way the original does, and put back
  // whatever the player was typing: commands parse their own arguments off this
  // global, so the text has to be *in* it rather than merely passed to it.
  char *line = ConsoleCommandLineBuffer();
  char *saved = ConsoleCommandLineSaveArea();
  std::snprintf(saved, ConsoleCommandLineSize, "%s", line);
  std::snprintf(line, ConsoleCommandLineSize, "%s", text.c_str());
  ConsoleExecuteCommandLine(line);
  std::snprintf(line, ConsoleCommandLineSize, "%s", saved);
}

// Replaces PumpQueuedConsoleCommand @ 0x004d6120 outright; the trampoline exists
// only so Detours has something to patch and is deliberately never called.
//
// One difference from the original, and it is a fix: this returns on an empty
// queue. The original walks into the sentinel and reads a payload off it - a
// List_Member_Base is 0xc bytes and has no `data` - which is only ever harmless
// because both of its callers test NumCommandsToExecute first.
void __stdcall HookedPumpQueuedConsoleCommand() {
  if (!ConsoleQueueGatesPass()) {
    return;
  }
  List<char *> *queue = ConsoleQueue();
  if (!queue || !queue->sentinel) {
    return;
  }
  List_Member_Base<char *> *first = queue->sentinel->next;
  if (!first || first == queue->sentinel) {
    return;
  }

  // Take the payload and unlink before running it, as the original does, so a
  // command that queues more - BATCH, or a message handler that sends - sees the
  // node already gone.
  char *&stored = entry_of(first)->data;
  std::string payload = stored ? stored : "";
  pool_free(stored);
  UnlinkFirstConsoleCommand(queue);

  DispatchConsolePayload(payload.c_str());
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

  std::string kind;
  std::string body;
  if (!json::OpenEnvelope(file, &kind, &body)) {
    // Nothing local can produce this: every writer wraps, and the queue hook
    // guards what they miss. So it came off the wire from a vanilla peer, or out
    // of a savegame written before the format changed. Run it as it stands,
    // since that is what it means and what it needs.
    //
    // This branch used to double as "a document that is not a string", which is
    // where the old format's ambiguity lived. It no longer has to guess: a bare
    // name cannot be an envelope (Json.h), so reaching here *is* the answer.
    static bool reported = false;
    if (!reported) {
      reported = true;
      Note("a peer is not running GkPlus - its payloads are bare file names, "
           "which will be run as such for the rest of the session, starting with",
           file);
    }
    return ExecuteCommandFile(file);
  }

  // Both of these carry a JSON string; anything else is a malformed envelope,
  // which only a peer running a broken build could send.
  if (kind == KindFile || kind == KindCommand) {
    std::string text;
    if (json::Classify(body.c_str(), &text) != json::Kind::String) {
      Note("payload body is not a string", file);
      return true;
    }
    // Back through the encoding boundary: fopen and the console both read a
    // char * through the active codepage, and the body held UTF-8.
    std::string ansi = GameTextFromUtf8(text.c_str());
    if (kind == KindFile) {
      // The engine has already set the current directory to Scripts, so this is
      // the call it was going to make.
      return ExecuteCommandFile(ansi.c_str());
    }
    // A command runs where it lands: it arrived already scheduled, so putting it
    // back on the console queue would cost it a frame and change the order it
    // was sent in.
    ExecuteConsoleCommand(ansi.c_str());
    return true;
  }

  if (kind == KindMessage) {
    // The *body*, not the envelope - a script's message_received sees what it
    // sent, which is what keeps the envelope invisible from JS.
    if (!Handler || !Handler(body.c_str())) {
      // Not on the console: a message arrives once per trigger firing on every
      // machine, and a level that ignores one would flood it.
      Note("message went undelivered", file);
    }
    // The return says "the file opened", and none of the engine's six callers
    // reads it. Consumed is the honest answer for a payload that was never a
    // file.
    return true;
  }

  // A kind this build does not know - a newer GkPlus at the other end. Dropped
  // rather than run: the one thing it must not become is an fopen of its own
  // JSON.
  Note("unknown payload kind, dropped", file);
  return true;
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
  if (IsEnvelope(stored)) {
    return stored;
  }
  if (repaired) {
    *repaired = true;
  }
  // Envelope and Quote both degrade to a document rather than failing, so this
  // branch cannot - which is what makes the return value unconditionally one.
  return FileEnvelopeFromGameText(stored);
}

std::string FileScriptPayload(const char *name) {
  return FileEnvelope(name ? name : "");
}

std::string MessageScriptPayload(const char *body_json) {
  return json::Envelope(KindMessage, body_json);
}

void SetScriptMessageHandler(ScriptMessageHandler handler) {
  Handler = handler;
}

bool QueueScriptPayload(const char *envelope) {
  if (!IsEnvelope(envelope)) {
    return false;
  }
  if (!QueueScriptExecution) {
    // Deliberately not falling back to the raw address: after DetourAttach that
    // entry point is the hook, which would wrap an envelope in another one.
    Note("not installed, payload dropped", envelope);
    return false;
  }
  QueueScriptExecution(envelope);
  return true;
}

bool QueueScriptMessage(const char *body_json) {
  if (!body_json || json::Classify(body_json) == json::Kind::Invalid) {
    return false;
  }
  return QueueScriptPayload(MessageScriptPayload(body_json).c_str());
}

ScriptQueueSystem::ScriptQueueSystem() {
  GetObjectAtOffset(QueueScriptExecution, 0x00505080);
  GetObjectAtOffset(RunQueuedScript, 0x00505310);
  GetObjectAtOffset(ApplyUpdateMessage, 0x004fde70);
  GetObjectAtOffset(ExecuteCommandFile, 0x0043f250);
  GetObjectAtOffset(PumpQueuedConsoleCommand, 0x004d6120);
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
  ::DetourAttach(&PumpQueuedConsoleCommand, HookedPumpQueuedConsoleCommand);
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
  ::DetourDetach(&PumpQueuedConsoleCommand, HookedPumpQueuedConsoleCommand);
  ::DetourDetach(&AddTriggerToGlobalList, HookedRegisterTriggers);
  ::DetourDetach(&ToRole, HookedToRole);
  ::DetourDetach(&ToReplaceDestructibility, HookedToReplaceDestructibility);
  ::DetourDetach(&CommandBatchAndBroadcast, HookedCommandBatchAndBroadcast);
  ::DetourDetach(&CommandVulnerability, HookedCommandVulnerability);
  ::DetourDetach(&MultiplayerRespawnRole, HookedMultiplayerRespawnRole);
  DetourDetach(&Associate, &AssociateAbi::HookedAssociate);
}
} // namespace gk
