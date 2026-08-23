#pragma once

#include <cstddef>
#include <iterator>

// Mirror of Rebellion's `List<T>` from the 3dc chunk library - the doubly-linked
// list that backs essentially every collection in Gunlok. Ground truth is the
// published Aliens vs Predator source, `3dc/win95/list_tem.hpp`; see the
// "Upstream Source" section of CLAUDE.md.
//
// The layout is what matters here: these are views over game memory, never
// objects we construct, so the AvP originals' constructors, destructor and
// mutators are deliberately NOT reproduced. `List<T>` stays a trivially-copyable
// standard-layout aggregate because the game passes it by value
// (AddTriggerToGlobalList takes a whole TriggerList in registers/stack, 0x10
// bytes of its 0x20-byte argument frame), and because it is embedded
// inside larger mirrors whose `offsetof` static_asserts must keep working.
//
//   List_Member_Base<T>   0x0c   { vptr, prev, next }
//   List_Member<T>        0x0c + sizeof(T), T at the first suitably aligned
//                                offset (0x0c for a pointer payload, 0x10 when
//                                T needs 8-byte alignment)
//   List<T>               0x10   { sentinel, n_entries, entry_pointers,
//                                  calculated_indices }
//
// THE trap this template exists to make unrepresentable: the list head is a bare
// `List_Member_Base`, so it has no `data` member. Walking with `cur->next !=
// sentinel` reads a node field off the sentinel and over-reads the heap; the
// correct termination is `cur != sentinel`, which is what begin()/end() below do.

namespace gk {

template <typename T> struct List_Member;

/// The part of a list node that carries no payload: vptr, `prev`, `next`.
/// 0x0c bytes, and the type of the **sentinel**, which is why a sentinel has no
/// `data` to read.
template <typename T> struct List_Member_Base {
  // AvP declares `virtual ~List_Member_Base() {}`, so every node - including the
  // sentinel - carries a vptr at offset 0x00. The engine calls it as a scalar
  // deleting destructor (`(**(code **)node->vtbl)(1)`) when unlinking entries.
  virtual ~List_Member_Base() = 0;

  List_Member_Base *prev; // 0x04
  List_Member_Base *next; // 0x08
};

/// A list node with its payload. `data` sits at 0x0c rounded up to
/// `alignof(T)`: 0x0c for a pointer, 0x10 for an 8-aligned value type, which
/// is what makes a `MenuListItem` node 0x78 rather than 0x10.
template <typename T> struct List_Member : List_Member_Base<T> {
  T data;
};

// The sentinel has no `data`, so only ever apply this to a node you have already
// proven is not the list head.
template <typename T>
inline List_Member<T> *entry_of(List_Member_Base<T> *node) {
  return static_cast<List_Member<T> *>(node);
}

/// The list header the engine embeds wherever it keeps a collection: 0x10
/// bytes of `{sentinel, n_entries, entry_pointers, calculated_indices}`.
///
/// A view over game memory, never something GkPlus constructs; there are no
/// mutators here on purpose. Range-for over one terminates on the sentinel
/// correctly by construction, which is the whole reason the template exists.
///
/// There is a **second, incompatible** list header in this binary whose
/// sentinel is behind a pointer, putting `count` at +0x04 instead of +0x0c.
/// Modelling one as the other shifts every field by 8, so check which you have
/// before embedding this. `CLAUDE.md` names the known instances.
template <typename T> struct List {
  using value_type = T;

  List_Member_Base<T> *sentinel; // 0x00 circular; ->next == ->prev == itself when empty
  int n_entries;                 // 0x04
  // Flattened array of pointers-to-payload, rebuilt lazily by operator[] and
  // pool-freed on every mutation. `mutable T **` in the original. It is owned,
  // but cannot be a pool_unique_ptr: List<T> has to stay trivially copyable for
  // the by-value AddTriggerToGlobalList call, and unique_ptr is move-only.
  T **entry_pointers;            // 0x08
  bool calculated_indices;       // 0x0c 1 = entry_pointers is up to date

  /// Forward iterator over the payloads, sentinel-safe: `end()` is the
  /// sentinel itself and is never dereferenced.
  struct iterator {
    using iterator_category = std::forward_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = T *;
    using reference = T &;

    List_Member_Base<T> *node{};

    reference operator*() const { return entry_of(node)->data; }
    pointer operator->() const { return &entry_of(node)->data; }

    iterator &operator++() {
      node = node->next;
      return *this;
    }
    iterator operator++(int) {
      auto copy = *this;
      ++*this;
      return copy;
    }

    bool operator==(const iterator &) const = default;
  };

  iterator begin() const { return {sentinel->next}; }
  iterator end() const { return {sentinel}; }

  int size() const { return n_entries; }
  bool empty() const { return sentinel->next == sentinel; }
};

// What pins `data`'s offset without an offsetof on a polymorphic type: the base
// is 0xc bytes, so T lands at 0xc rounded up to alignof(T).
static_assert(sizeof(List_Member_Base<void *>) == 0xc);
static_assert(sizeof(List_Member<void *>) == 0x10);
static_assert(sizeof(List<void *>) == 0x10);

} // namespace gk
