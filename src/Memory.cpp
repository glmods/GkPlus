#include "Memory.h"

#include "Core.h"

namespace gk {
// pool_alloc @ 0x00571470 / pool_free @ 0x005715b0 - the game's page
// sub-allocator over the CRT (see the Memory.h header comment). Resolved
// per-call; GetBaseAddress caches, so this is a cached read plus an add.
void *pool_alloc(size_t sz) {
  CDecl<void *, size_t> fn;
  GetObjectAtOffset(fn, 0x00571470);
  return fn(sz);
}

// __cdecl, NOT __stdcall - it ends in a bare RET at 0x0057166f while taking one
// stack argument, and every game call site cleans up itself (PumpQueuedConsoleCommand
// does `CALL free @ 0x005e3f7b` then `ADD ESP,0x4`).
//
// This was declared StdCall here for a long time and cost nothing, because almost
// nothing called it: pool_unique_ptr's deleter is empty, so the only callers were
// rare conversion paths. Calling a __cdecl function through a __stdcall pointer
// leaks **4 bytes of stack per call** - the compiler emits `sub esp,4` afterwards to
// undo a callee pop that never happens - so ESP drifts down until the caller's
// epilogue returns to garbage. The moment anything called it once per frame it
// became a non-deterministic access violation with EIP on the stack.
void pool_free(void *ptr) {
  CDecl<void, void *> fn;
  GetObjectAtOffset(fn, 0x005715b0);
  fn(ptr);
}
} // namespace gk
