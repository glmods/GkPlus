#pragma once

#include <string>

namespace gk {
// The engine's script channel, carrying JSON instead of bare file names - in the
// fields that hold a script name as well as on the queue itself.
//
// Gunlok has one channel for "something happened, react to it": a trigger fires
// and QueueScriptExecution @ 0x00505080 puts a .gcs file name on ScriptQueue
// @ 0x007ba35c *and* broadcasts it as update 0x67, so every machine runs its own
// local copy of that file. Two consumers pop it, both on the main thread:
// RunQueuedScript @ 0x00505310 (the host, one per frame) and the `case 0x67` arm
// of ApplyUpdateMessage @ 0x004fde70 (joiners, unthrottled). See
// `threading_model_notes.md` and `directplay_protocol_notes.md` §8.11.
//
// The payload is redefined as **a JSON document**:
//
//   * a JSON **string** is the legacy behaviour - it names a .gcs, which is run
//     exactly where and when the engine would have run it;
//   * **anything else** is a message, handed to the handler below instead of to
//     the file system. `CustomLevel` routes it to a script level's
//     `message_received`.
//
// **The encoding happens where the value is written, not where it is queued.**
// Four hooks - RegisterTriggers, PickupActor::Associate, ToRole and
// ToReplaceDestructibility - quote the bare name the engine hands them, so a
// script-name field holds a document from the moment it is set. Every field that
// reaches the queue is written through one of those, by the vulnerability sweep
// below, or by GkPlus's own bindings, which encode in `js::ToScriptPayload`.
//
// Neither side has to guess what it is holding. A game-side write is always a
// bare name (a console argument or a GLS field); GkPlus's own writes arrive inside
// an EncodedPayloadScope, which says "already a document, pass it through". That
// is what replaced an earlier marker byte in the stored value: with every writer
// encoding, there are no longer two representations to tell apart.
//
// **This is deliberately visible on disk.** SaveGame writes a trigger's script
// name verbatim (`save_system_notes.md`), so a save written by this build carries
// JSON and will not load correctly in an unpatched Gunlok. Reading an *older*
// save still works: a bare name coming back through ReadActorFixups takes the
// residual path below.
//
// Some sites queue a name without ever storing it, so there is no field to
// convert and the only way to stop a bare name existing is to replace the body.
// `CommandBatchAndBroadcast` and `MultiplayerRespawnRole` are reproduced call for
// call, encoding the name at the site and dropping the allocation the original
// leaked. `CommandVulnerability` is wrapped instead and its result swept, because
// reproducing 1369 bytes of argument parsing to change one string is a worse trade
// than checking the field afterwards.
//
// `OnFlagCaptured` is deliberately left alone. Its 760 bytes drive two
// BroadcastToPlayers payloads, five TeamSlots writes, a vtable getter/setter pair
// and a special case for the role named "Hark"; duplicating its team-to-script
// switch a few instructions earlier would only add a way to pick the wrong file.
// Its name is encoded where every other stray one is - by ScriptQueuePayload, at
// the queue hook, which is still before the payload reaches the queue proper.
//
// What is left holding a bare name when ScriptQueuePayload sees it: that one site,
// a peer **without GkPlus** broadcasting update 0x67, and a savegame written
// before this change. Only the first is ours, and by choice.
//
// ScriptQueuePayload quotes all of those at the hook, the way it always did.
//
// **Payloads are UTF-8, the engine's strings are ANSI, and `Encoding.h` is the
// seam.** A name is transcoded on the way into a payload and back on the way to
// ExecuteCommandFile, because `fopen` reads a `char *` through the active
// codepage while JSON - and both of the decoders that read these payloads - want
// UTF-8.
//
// Nothing here needs the script host: the classification is `gk::json`, and the
// queue hook could not use QuickJS anyway - it runs on the **executor thread**,
// where the JSContext does not belong. Only the delivery half is main-thread,
// which is what makes calling into JS from the handler safe. That holds by call
// graph, not by convention: `RunQueuedScript` has one caller (the in-game tick),
// and `ApplyUpdateMessage` has one (`ClientReceivePump`) whose own three callers
// are the tick, the multiplayer lobby pump and `UpdateAndDrawMenuScreen` - all
// main-thread.
//
// The last of those is worth knowing: an update can be applied **from the front
// end**, so a message can arrive with no level loaded at all. It is then reported
// undelivered rather than queued for later, matching what the engine does with a
// script file nobody is around to care about.

// --- writing a script-name field ------------------------------------------------
//
// The engine has four fields that hold "the script to run when this happens":
// TriggerData::script_name @ +0x54, PickupActor::associated_script @ +0x134,
// Vulnerability::script @ +0x10 (via Role::interface_beam_script @ +0x88) and
// ReplaceDestructibility::script @ +0x08. Each is a plain strdup'd C string that
// ends up at QueueScriptExecution.
//
// GkPlus writes documents into two of them through the engine's own setters -
// RegisterTriggers and Associate - which are hooked to quote what they are given.
// Scoping a call says "this one is already encoded, leave it alone". Main-thread
// only, and it must wrap the call itself rather than being set and forgotten.
class EncodedPayloadScope {
public:
  EncodedPayloadScope();
  ~EncodedPayloadScope();

  EncodedPayloadScope(const EncodedPayloadScope &) = delete;
  EncodedPayloadScope &operator=(const EncodedPayloadScope &) = delete;

private:
  bool outer_;
};

// Whether a script-name write is currently inside such a scope. Read by the
// writer hooks; of no use to anything else.
bool PayloadIsEncoded();

// What a script-name field holds -> what goes on the queue, as a pure function so
// a harness can check it outside the game. **The return value is always a valid
// JSON document**, which is the invariant the whole subsystem rests on.
//
// After the writer hooks this is a pass-through for everything they touch. It
// still quotes, because of the residual sources listed above, and `*repaired`
// reports when it had to - which for a build with no vanilla peers and no old
// saves in play means "something wrote a field without encoding it".
std::string ScriptQueuePayload(const char *stored, bool *repaired = nullptr);

// Receives a payload that was a JSON document but not a string. `json` is the
// raw document text, valid only for the duration of the call. Returns whether
// anything took it, which is only used for diagnostics.
using ScriptMessageHandler = bool (*)(const char *json);

// Installs the sink for non-string payloads; null drops them. Set by
// CustomLevelSystem, whose handler is DispatchCustomLevelMessage.
void SetScriptMessageHandler(ScriptMessageHandler handler);

// Queues `json` as a payload and broadcasts it, exactly as a trigger's script
// name would be: the local machine delivers it through RunQueuedScript and every
// other player through update 0x67. `json` must be a complete JSON document -
// false means it was not, or that the subsystem is not installed.
//
// A JSON *string* is legal and does what it says: it runs that .gcs everywhere,
// which is what a trigger firing a file already does.
//
// Two caveats inherited from the engine, neither of which this layer papers
// over. The local copy is popped by RunQueuedScript at **one per frame**. And
// the broadcast half runs on the calling thread, whereas the game only ever
// broadcasts from the executor - harmless in single player, where it is a push
// onto the loopback queue under its own lock, but a multiplayer host sends
// DirectPlay traffic from wherever this is called.
bool QueueScriptMessage(const char *json);

// Hooks both ends of the channel. The four writers above encode a name as it is
// stored; QueueScriptExecution guards the invariant for what they do not cover;
// RunQueuedScript and ApplyUpdateMessage mark a popped payload as it travels to
// ExecuteCommandFile, which is the one place it is consumed. RAII, like every
// other *System - construct and destroy inside a Detours transaction.
class ScriptQueueSystem {
public:
  ScriptQueueSystem();
  ~ScriptQueueSystem();
};
} // namespace gk
