# The vertex-conversion differential test

`src/VertexFormat.cpp` against **itself at a git revision**, natively, with no Gunlok and no
Vulkan. It is the check for any change to `ConvertVertices`, `FvfStride`, `FvfSupported` or
`PositionBounds`.

```powershell
.\utils\vfdiff\run.ps1               # current tree vs HEAD
.\utils\vfdiff\run.ps1 -Ref HEAD~3   # vs any revision
.\utils\vfdiff\run.ps1 -SelfTest     # ... and prove the harness can fail
```

Exit 0 means every case matched. 11,337 cases at the time of writing.

**This file exists because the harness was written twice and lost twice**, both times to a session
scratchpad (`vulkan_renderer_notes.md` §4.82, §4.84). It is thirty lines of scaffolding around the
one property that matters, and rebuilding it costs more than keeping it.

## What it does

`run.ps1` extracts `src/VertexFormat.{h,cpp}` at `-Ref`, rewrites `namespace vulkan` to
`namespace refvulkan` in the copy, and links both into one binary so the two implementations can be
called side by side on the same inputs. `main.cpp` then sweeps:

- every FVF the layout encoding can express - both position types x normal x diffuse x specular x
  0/1/2 texture coordinate sets - and the same set again with **unrelated bits set**, which is what
  the masked dispatch is supposed to absorb;
- four strides each: the FVF's own, and three padded ones, since a user-pointer draw states its own
  stride and is entitled to pad;
- six vertex counts including 0;
- the rejection and null-argument paths, and a stride shorter than the FVF implies.

Two things about the comparison are deliberate:

- **`memcmp`, not a float epsilon.** This is a byte transform. Anything it does differently is a
  defect, not a rounding difference, and an epsilon would hide exactly the field-offset mistakes
  this is for.
- **A guard vertex past the end of each buffer**, filled with `0xAA` and required to still match.
  A conversion that writes one vertex too many is otherwise invisible.

## Why this file and not a unit test

`src/VertexFormat.cpp` is the one part of the renderer with no game dependency at all - no
`GetBaseAddress()`, no D3D, no Vulkan - which is why it can be exercised this way and why the
header has always said so. Everything else in `src/` faults outside Gunlok
(`harness_testing_notes.md`).

A differential test rather than expected values because the expected values *are* the previous
implementation: the point is always "did this refactor change any output", and `ConvertRun` is
written once and instantiated eight ways precisely so the specialized paths cannot disagree with
the generic one. This is what checks that they do not.

## The rule

**Confirm the harness can fail before trusting a pass.** `-SelfTest` does it for you: it perturbs
`v.pos[3]` in one branch, rebuilds, and requires a non-zero exit. A harness that cannot fail proves
nothing, and this one has an obvious way to become vacuous - if the re-namespacing ever stops
applying, both halves resolve to the same symbol and every case passes trivially.
