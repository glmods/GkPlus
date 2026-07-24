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

void pool_free(void *ptr) {
  StdCall<void, void *> fn;
  GetObjectAtOffset(fn, 0x005715b0);
  fn(ptr);
}
} // namespace gk
