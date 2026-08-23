#pragma once

#include <cstddef>
#include <iterator>

// Mirror of Rebellion's `HashTable<T>` (`_base_HashTable`) from the 3dc chunk
// library. Ground truth is the published Aliens vs Predator source,
// `3dc/win95/Hash_tem.hpp`; see the "Upstream Source" section of CLAUDE.md.
//
// Separate chaining, power-of-two table, no rehashing - the table size is fixed
// at construction and the mask is `size - 1`.
//
//   HashTableBase<T>       0x10   { n_entries, table_size, table_size_mask, chains }
//   HashTable<T>           0x14   the same, behind a 3-slot vtable
//   HashTableBase<T>::Node        { d, next } - payload FIRST, link second, which
//                                 is the opposite order from List_Member<T>
//
// Two shapes because Gunlok uses two. The virtuals are the v1.1 "node allocation
// policy" the AvP header documents as added after v1.0 (`NewNode`, `DeleteNode`,
// `NewNode(T, Node*)` - in that declaration order, so that is the slot order);
// tables that never need them are the plain base. As always these are views over
// game memory, so none of the originals' mutators are reproduced.

namespace gk {

/// The vptr-less hash-table header: separate chaining, power-of-two table, no
/// rehashing, `n_entries` at +0x00. `roles` @ 0x007b48f0 is one of these.
///
/// Gunlok uses **both** shapes and they differ by four bytes on every field:
/// HashTable carries the three-slot node-allocation vtable and starts its
/// fields at +0x04. Check which you have.
template <typename T> struct HashTableBase {
  // AvP's `class Node { TYPE d; Node *nextP; }`. Note the payload comes first:
  // a chain walk reads `*node` for the value and `node[1]` for the link, which is
  // how these show up in decompilation.
  struct Node {
    T d;
    Node *next;
  };

  unsigned n_entries;      // 0x00
  unsigned table_size;     // 0x04 always a power of two
  unsigned table_size_mask;// 0x08 == table_size - 1; the bucket index is hash & this
  Node **chains;           // 0x0c array of `table_size` chain heads

  // Walks every chain of every bucket, skipping empty ones. Unlike List<T> there
  // is no sentinel to fall off - a chain simply ends at a null `next`.
  class iterator {
    Node **bucket_{};
    Node **bucket_end_{};
    Node *node_{};

    void seek() {
      while (!node_ && bucket_ != bucket_end_) {
        node_ = *bucket_++;
      }
    }

  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = T *;
    using reference = T &;

    iterator() = default;
    iterator(Node **first, Node **last) : bucket_(first), bucket_end_(last) {
      seek();
    }

    reference operator*() const { return node_->d; }
    pointer operator->() const { return &node_->d; }

    // The node itself, for callers that need the link (or the node's address as
    // an identity). Null once the iterator is exhausted.
    Node *node() const { return node_; }

    iterator &operator++() {
      node_ = node_->next;
      seek();
      return *this;
    }
    iterator operator++(int) {
      auto copy = *this;
      ++*this;
      return copy;
    }

    bool operator==(const iterator &other) const {
      return node_ == other.node_ && bucket_ == other.bucket_;
    }
  };

  iterator begin() const {
    if (!chains) {
      return {};
    }
    return {chains, chains + table_size};
  }
  iterator end() const {
    if (!chains) {
      return {};
    }
    return {chains + table_size, chains + table_size};
  }

  unsigned size() const { return n_entries; }
  bool empty() const { return n_entries == 0; }
};

// The polymorphic form. MSVC puts the vptr at 0x00 and the (non-polymorphic) base
// subobject straight after it, which is what moves n_entries to 0x04.
template <typename T> struct HashTable : HashTableBase<T> {
  using Node = typename HashTableBase<T>::Node;

  virtual Node *NewNode() = 0;              // slot 0
  virtual void DeleteNode(Node *) = 0;      // slot 1
  virtual Node *NewNode(T, Node *) = 0;     // slot 2 (private in AvP)
};

static_assert(sizeof(HashTableBase<void *>::Node) == 0x8);
static_assert(sizeof(HashTableBase<void *>) == 0x10);
// The vtable adds exactly one pointer at the front and nothing else.
static_assert(sizeof(HashTable<void *>) == sizeof(HashTableBase<void *>) + 4);

} // namespace gk
