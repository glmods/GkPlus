#pragma once

namespace gk {
struct Actor;

// Lets a console command be handed an **Actor pointer** where its handler would
// otherwise resolve a name out of the token table.
//
// The problem this solves is a JavaScript one. The command-backed half of the
// `gk` surface dispatches the game's own handlers, which is what keeps their
// defaults, clamps, executor handshake and broadcasts exactly right (see
// src/JsCommands.cpp). But ~27 of those handlers name their actor through
// `ConsoleParseActorName` @ 0x004d6d90, so the only way to reach one used to be
// a token name - which meant an actor the level did not place, a spawned one,
// could not be addressed at all, and a script had to speak names instead of
// objects.
//
// The seam is that one shared helper. It is `bool __fastcall(Actor **out)`, and
// two properties make it substitutable rather than merely hookable:
//
//   * it calls `ConsoleParseWord` **unconditionally and first**, so the word
//     cursor advances whether or not the name resolved - a substituted call
//     leaves the parse position exactly where the handler expects it; and
//   * it prints nothing on failure, so a word that resolves to no token costs
//     no console line.
//
// So a substituted argument is spelled on the command line as the actor's
// **decimal id**, and the override supplies the pointer. That spelling is
// deliberately not a reserved placeholder, because it also makes the *other*
// group work with no second mechanism: the handlers that read their actor with
// `ConsoleParseInt` @ 0x004d6770 (`ANIM`, `BOARD`, `DEFOGGER`, `FOGGER`,
// `REMOVEBB`, `SPEAK`, the vision/hearing pair, `WATCH`) parse those same digits
// directly and never consult the override. One code path covers both, and no
// caller has to know which group its command belongs to.
//
// The override is per-thread and consumed in order, so a handler taking several
// actors (`SET SPEED`, `SET LOOP TIME`) works by pushing them in argument order.
// Anything left unconsumed when the scope ends is dropped rather than leaking
// into the next command.
class ActorArgScope {
public:
  ActorArgScope();
  ~ActorArgScope();

  ActorArgScope(const ActorArgScope &) = delete;
  ActorArgScope &operator=(const ActorArgScope &) = delete;

  // Queues one actor, in the order its argument appears on the command line.
  void Push(Actor *actor);

private:
  // Nesting is not expected - a dispatch does not run another - but restoring
  // rather than clearing means a nested one cannot strand the outer queue.
  unsigned saved_begin_;
  unsigned saved_cursor_;
};

// Installs the ConsoleParseActorName detour. Hook-only, no scripting surface;
// inert until an ActorArgScope arms it, so the game's own command dispatch is
// unaffected.
class ActorArgSystem {
public:
  ActorArgSystem();
  ~ActorArgSystem();
};
} // namespace gk
