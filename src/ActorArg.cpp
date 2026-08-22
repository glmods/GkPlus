#include "ActorArg.h"

#include "Core.h"
#include "DetourUtils.h"

#include <vector>

namespace gk {
namespace {

// The pending substitutions for the dispatch running on this thread, and how far
// the handler has got through them. `thread_local` because a console command runs
// on whichever thread dispatched it - the script host is main-thread, but
// `CommandFrag` is reachable from the executor - and an override armed on one
// thread must never be consumed by a handler running on the other.
thread_local std::vector<Actor *> Pending;
thread_local unsigned Cursor = 0;

// `ConsoleParseActorName` @ 0x004d6d90 is `bool __fastcall(Actor **out)`: the
// out-pointer is the only argument and it arrives in ECX, with nothing on the
// stack. That is exactly the shape a member function pointer models, which is
// the trick src/InputFix.cpp uses for the same reason - so `this` here *is* the
// caller's `Actor **`.
struct ActorOutParam {
  bool HookedParse();
};
bool (ActorOutParam::*ConsoleParseActorName)() = nullptr;

} // namespace

bool ActorOutParam::HookedParse() {
  // The original runs first, always. It is what consumes the word, and the
  // handler's next ConsoleParse* call depends on that having happened - so this
  // is not an optimisation to skip when an override is pending.
  const bool parsed = (this->*ConsoleParseActorName)();

  if (Cursor >= Pending.size()) {
    return parsed;
  }
  Actor *substitute = Pending[Cursor++];
  if (!substitute) {
    // A null in the queue means "this argument was not an actor after all";
    // leave the real parse alone rather than reporting a resolve that failed.
    return parsed;
  }
  *reinterpret_cast<Actor **>(this) = substitute;
  // True regardless of what the parse found. Several of these handlers branch on
  // the result to try a *different* reading of the same word, and forcing the
  // actor branch is the whole point of being here.
  return true;
}

ActorArgScope::ActorArgScope()
    : saved_begin_(static_cast<unsigned>(Pending.size())),
      saved_cursor_(Cursor) {
  // Push/consume happen above whatever an enclosing scope left, so the outer
  // one's cursor stays meaningful.
  Cursor = saved_begin_;
}

ActorArgScope::~ActorArgScope() {
  // Anything this scope queued and the handler did not consume is discarded. A
  // command that takes fewer actors than were pushed is normal - `SET SPEED`
  // reaches one of two call sites - and a leftover must not be inherited by the
  // next command to run on this thread.
  Pending.resize(saved_begin_);
  Cursor = saved_cursor_;
}

void ActorArgScope::Push(Actor *actor) { Pending.push_back(actor); }

ActorArgSystem::ActorArgSystem() {
  GetObjectAtOffset(ConsoleParseActorName, 0x004d6d90);
  DetourAttach(&ConsoleParseActorName, &ActorOutParam::HookedParse);
}

ActorArgSystem::~ActorArgSystem() {
  DetourDetach(&ConsoleParseActorName, &ActorOutParam::HookedParse);
}
} // namespace gk
