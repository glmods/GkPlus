#pragma once

#include <memory>

namespace gk {
void *pool_alloc(size_t sz);
void pool_free(void *ptr);

template <typename T> struct pool_deleter {
  void operator()(T *ptr) {
    ptr->~T();
    ::gk::pool_free(ptr);
  }
};

// Marks a pointer field of a game-struct mirror as *owned*, i.e. the containing
// object's destructor releases it through pool_free. Costs nothing at runtime: the
// deleter is empty, so the member stays pointer-sized and the struct's
// static_asserts keep holding.
//
// There is only ONE heap on the game side. `pool_alloc` @ 0x00571470 is a
// page-based sub-allocator layered over the CRT (it falls back to the real CRT
// malloc @ 0x00601f4a for large blocks, and pool_free hands a page back to the
// real CRT free @ 0x00601f2d once it empties), and the game's own malloc/free
// symbols are bare JMP thunks into it:
//
//   malloc     @ 0x005e3f72 -> JMP pool_alloc
//   free       @ 0x005e3f7b -> JMP pool_free
//   free_sized @ 0x005e3f64 -> CALL pool_free (sized wrapper; discards the size)
//   strdup     @ 0x0044e1a0 -> game-written, allocates through that malloc thunk
//
// So a decompiled `free(x)` and a decompiled `free_sized(x, n)` are the same call,
// and strings are pool memory like everything else - there is no second
// "CRT-allocated" category to model, and a deleter calling this DLL's own ::free
// would be wrong for every pointer here (our /MD UCRT heap is neither the pool nor
// the game's CRT heap).
//
// What a raw pointer beside a pool_unique_ptr sibling does mean - always keep the
// reason in a comment:
//   * refcounted    released by a per-type Release function, or by "decrement,
//                   then call slot 0 with 1"
//   * borrowed      owned elsewhere; the localized string table (GetResourceString)
//                   and the roles hash are the two big sources
//   * conditional   ownership depends on a sibling flag, so it cannot be a
//                   unique_ptr (MenuItemData::label, VulnList payloads)
//   * leaked        allocated per-object but never released
template <typename T>
using pool_unique_ptr = std::unique_ptr<T, pool_deleter<T>>;

// For an owned buffer whose contents are not modelled. `pool_deleter<void>` cannot
// run a destructor, which is exactly right: nothing the pool hands out is a
// constructed C++ object unless a dtor is called on it explicitly.
template <> struct pool_deleter<void> {
  void operator()(void *ptr) { ::gk::pool_free(ptr); }
};

// Owned NUL-terminated string. Every engine string is allocated with the strdup
// above or with a malloc+memcpy pair, and freed through the same pool.
using pool_string = pool_unique_ptr<char>;

// TODO: handle aligned allocation
template <typename T, typename... Args> auto make_pool_unique(Args &&...args) {
  void *raw = ::gk::pool_alloc(sizeof(T));
  if (!raw) {
    throw std::bad_alloc{};
  }
  try {
    T *val = new (raw) T(std::forward<Args>(args)...);
    return pool_unique_ptr<T>(val);
  } catch (...) {
    ::gk::pool_free(raw);
    throw;
  }
}
} // namespace gk