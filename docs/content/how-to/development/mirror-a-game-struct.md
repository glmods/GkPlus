---
title: "Mirror a game struct and prove its layout"
description: "Model a decompiled Gunlok struct in a src/ header so that a wrong offset is a compile error rather than a fault somewhere unrelated."
weight: 30
audience: ["developer"]
---

This guide shows a **developer** how to add a struct mirror to `src/` and make the compiler prove
it. The layout comes from the binary; see
[Reverse-engineer a function in Ghidra](/how-to/development/reverse-engineer-in-ghidra/) if you do
not have it yet, and `address_map.md` if you suspect it is already recorded.

## Write the struct

Put it in the subsystem's header, in `namespace gk`, next to the free functions that use it.

**If the object has a vtable**, declare the slots as declaration-ordered pure virtuals and let the
implicit vptr occupy `0x00`; the first data member then starts at `0x04`. Never add an explicit
`void *vtbl` member: it double-counts those four bytes and hides the slots. Name a slot whose
purpose you do not know `StubN()`, as `Actor::Stub27` does. Adding a virtual costs no object size,
so the size assertion still holds and will catch a mistake.

**If a second vptr appears mid-struct**, that is multiple inheritance, so model it with real C++
inheritance instead of a `void *sub_vtbl` field. MSVC lays base subobjects out in declaration
order, so a correctly sized first base puts the second vptr at the right offset on its own:
`Map : MapBase, RefCountedBase` puts `RefCountedBase`'s vptr at `0xa4` because
`sizeof(MapBase) == 0xa4`.

**If you need a vtable's slot count**, MSVC emits every class's vftables adjacently in `.rdata`, so
the table ends at the next address referenced *at all*. Walk the candidate slots counting references
per dword: a real slot has zero, a boundary has some. "Looks like a function pointer" is not a slot
test: past a table's end sit one-slot list/node vtables, then floats.

## Use the container mirrors instead of open-coding them

- A `{sentinel, count, cached_array, cache_valid}` group is **`List<T>`** from `src/List.h`. Embed
  it as one member and model nodes as `List_Member<T>`. Picking `T` is a real claim: `List_Member<T>`
  puts `data` at `0x0c` for a pointer and `0x10` for an 8-aligned value type.
- **There is a second, incompatible list header in this binary**, pointer-first:
  `{List_Member_Base *sentinel; int count; void *cache; bool valid;}`, where the sentinel is a
  separate heap object and `count` sits at `+0x04` instead of `+0x0c`. Modelling one as the other
  shifts every field by 8. Known instances are the spark emitter list at `DAT_007ba1b4` and both
  Unit shadow lists. Check which one you have before embedding `List<T>`.
- A hash table is `HashTable<T>` or `HashTableBase<T>` from `src/HashTable.h`, and the difference is
  four bytes of offset on every field. `HashTable<T>` carries the three-slot node-allocation vtable
  and puts `n_entries` at `0x04`; `HashTableBase<T>` has no vptr and starts at `0x00`. The node is
  `{d, next}`, payload first, the opposite of `List_Member<T>`.

## Annotate ownership

An owning pointer is `pool_unique_ptr<T>` (`src/Memory.h`), and an owned string is the `pool_string`
alias. Read the containing object's destructor before deciding, and leave a comment saying why a
sibling stayed raw: **refcounted**, **borrowed**, **conditional**, or **leaked**.

Do not give these a CRT-flavoured deleter. There is one heap on the game side (`pool_alloc` is a
page sub-allocator over gl.exe's CRT, and the game's `malloc`/`free`/`strdup` are thunks into it),
and this DLL's `/MD` UCRT heap is a different heap. The deleter is empty, so the member stays
pointer-sized and the existing assertions remain the proof.

## Type the fields

- An enum-like `int` becomes `enum class Name : int` applied to the field, but **only encode values
  you have verified**, from the game's own enum or a keyword→id function. Leave it `int` rather than
  guess a mapping.
- A field with a known offset and unknown meaning is `field0xNN`, or `unkN[...]` padding; an unknown
  accessor is `GetField0xNN` / `SetField0xNN`.
- If a name turns out to be wrong, **rename it** instead of annotating around it, in one pass
  across the `src/` mirror, every `*_notes.md`, and the Ghidra database, then grep the old name to
  confirm nothing dangles.

## Assert the layout

This is the step that makes the mirror worth having. Write, at minimum:

```cpp
static_assert(sizeof(Thing) == 0x…);
static_assert(offsetof(Thing, some_field) == 0x…);
```

Pin at least one `offsetof` per base for a multiply-inherited type, plus the size of each base.
Those are what prove the split, and they fail loudly if it is wrong. `src/Map.h` and `src/Roles.h`
are the worked examples.

Then build. `cmake --build build` is incremental and quick, and the assertions are the check, so
do not eyeball the offsets:

```powershell
cmake --build build 2>&1 | Select-String ': (warning|error):' | Select-String -NotMatch 'invalid-offsetof'
```

The `-Winvalid-offsetof` warnings on `Actor`, its subclasses and `Map` are pre-existing and benign:
every struct modelled with pure virtuals is non-standard-layout, which is the expected cost of the
convention above and not a signal to switch back to an explicit vtbl field. `List<T>` is
deliberately a trivially-copyable, standard-layout aggregate, so embedding it adds none.

## If the struct is shared with a shader

Do not hand-write the offsets. Add the pair to `PAIRS` in `src/gen-shader-abi.py`, which parses the
`src/shaders/*.slang` declaration and generates an `offsetof` per field plus a `sizeof` per struct
into the build tree. Adding or reordering a field on one side only is then a compile error naming
the field. A hand-written `offsetof` cannot replace it: a field permutation preserves `sizeof` and
every assertion that pins a field *after* the disturbance.

## Record it

Put the offsets and the reasoning where the next reader will look: `address_map.md` for addresses
and offsets, the subsystem's `*_notes.md` for behaviour, and the Ghidra database for the type
itself. A layout that lives only in a header is one nobody can check against the binary.

## Related

- [Add a subsystem](/how-to/development/add-a-subsystem/): where a new header and its `.cpp` go.
- `address_map.md`, `actor_vtable_notes.md`, `role_system_notes.md`: the recorded layouts.

## Reference and background

- [Namespace map](/reference/cpp/namespaces/): which header a new mirror belongs in.
- [C++ API](/reference/cpp/): the generated reference, and the one thing it structurally
  cannot carry: a description for a struct field. The header stays the only place to read
  one.
- [Design records index](/reference/data/notes-index/): `address_map.md`,
  `actor_vtable_notes.md` and the rest, and what each holds.
- [Reading a binary that cannot answer back](/explanation/reading-a-binary-that-cannot-answer-back/):
  why a layout is proved with `static_assert` instead of eyeballed, and why "this field
  has no writer" is the hardest claim to make.
