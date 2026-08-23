---
title: "Why the renderer seam is the device"
description: "A second renderer has to intercept somewhere total. The engine's own render queue is not, and the measurement that settled it."
weight: 60
audience: ["developer"]
---

This page is for developers working on the Vulkan renderer or wondering why the capture layer looks
the way it does. It is about one question, where you cut to replace a renderer inside a program you
cannot recompile, and about the properties the answer forced on everything downstream.

## Gunlok has a renderer already, and it looks like the obvious seam

Gunlok's high-level rendering lives in a separate library, AWAPI, whose frame is "submit everything,
then drain once". Around a hundred producers push work into one global `RenderQueue` via
`RenderQueue_Submit`, and `RenderQueue_Flush` state-sorts the result by material and then by texture,
drawing a back-to-front list last.

That is an extremely attractive place to hook. It is high level, it carries intent rather than state,
and one function sees the frame.

It is also not total, which is the only property that matters.

## The measurement

`rendering_notes.md` §4.1 counted it. Of the nine functions that call the four `Aw_Draw*` wrappers,
**only two are downstream of `RenderQueue_Flush`**. Text, particles, the in-game menus, the shadow
renderer and the world-effect overlays all draw immediately and are invisible to a queue hook. The
queue is not even flushed once per frame, since `SubmitAndFlushMapGeometry` drains it itself, and the
inventory screen *replaces* the world submit rather than overlaying it.

The next candidate down, hooking `CreateVertexBuffer` to catch geometry as it is made, fails for a
different reason: two of the four wrappers are user-pointer draws, so for those no
`IDirect3DVertexBuffer8` ever exists.

What *is* total is the device. Exactly four call sites in the whole binary reach an
`IDirect3DDevice8::Draw*` slot, all four inside those wrappers, and they are the only functions that
also reference the `direct3d_device` global at `0x007c121c` (`src/D3D8Capture.h`).

So the seam is the D3D8 device, and therefore `Direct3DCreate8`, which is also, by coincidence of a
kind, the DLL's only export and the reason GkPlus is loaded at all (see
[Why GkPlus is a d3d8.dll](/explanation/why-gkplus-is-a-d3d8-dll/)). The door and the seam are the
same hole.

## What that buys, and what it takes away

**It buys totality.** If the game drew it, the seam saw it. There is no producer to discover later,
no path that bypasses the interception, no "except for text" caveat. For a renderer replacement,
that is worth more than any amount of semantic richness.

**It takes away intent.** The capture layer only *records*. It never implements D3D8 semantics on
anything. It keeps a flat struct of fixed-function state and, per draw,
snapshots the parts that matter into a `DrawItem`.

What it therefore does not have is a scene graph, an object identity, or any notion of what a draw
*is*. A draw is a primitive type, a vertex range, a stack of fixed-function state, and a set of bound
textures. The most meaningful name attached to it is the asset name of the texture in stage 0.

That constraint is visible in the shape of features far downstream. `render.material.override` is
keyed on a `.rim` name because that name is the only identity a draw carries. Lighting maps are
served by file-name convention (`<texture> lighting.dds`) with nothing registering them, for the
same reason: at this seam, a texture's asset name is the only handle there is.

## The switch is one branch

Under `GKPLUS_RENDERER=vulkan`, the capture device's `Present` returns `D3D_OK` **without
forwarding** and calls `vulkan::DrawFrame()` instead. The game keeps its D3D9 device and every
resource it created; nothing is invalidated and nothing is torn down. Its output simply stops
reaching the screen (`src/VkRenderer.h`).

This is why turning the Vulkan renderer on takes no preparation: there is one branch, and either
side of it is a complete renderer.

**Frame pacing is inherited**, because `DrawFrame` runs inside the game's own present call. That is
mostly a gift and occasionally a trap: it is why a level load, whose progress bar repaints once per
role converted, around 830 presents a load, turned out to cost *presents × cost-of-one-present* rather
than anything to do with the work being done, and why dropping most of those presents needed a
subsystem of its own.

Two threads reach this seam, and the original comment saying otherwise was wrong and had never been
checked: the executor thread creates, locks and releases vertex buffers. That correction is recorded
in `src/D3D8Capture.h` rather than quietly fixed, which is the right treatment for an assumption that
went unexamined.

## The reference is the original runtime, not the translation layer

By default the capture layer forwards to `d3d8to9`, a D3D8-on-D3D9 translation layer. For a long
time that was also what renderer comparisons were measured against, and it was the wrong reference.

Windows still ships a 32-bit `d3d8.dll` in SysWOW64, so the original runtime is one `LoadLibrary`
away, and `GKPLUS_RENDERER=d3d8` selects it. The reason this matters is stated bluntly in the source:
a defect the translation layer *shares* with our renderer measures as a perfect match. The notes
record a junk-pile decal on level02 matching `d3d9` to 0.1 mean RGB and not matching the real
thing, and `d3d8to9` dropping two HUD columns the game genuinely draws.

Roughly thirty sections of measurement were taken against `d3d8to9` before anyone checked whether the
original was available. That is the single most expensive methodological mistake recorded in the
renderer notes, and what it got wrong was the reference rather than the rendering.
[What a residual can and cannot say](/explanation/what-a-residual-can-and-cannot-say/) is the rest of
that argument.

## What was given up in choosing this seam

Being honest about the trade: hooking the device means reimplementing the fixed-function pipeline.
Everything the D3D8 runtime did for free (texture stage combiners, lighting, fog, the D3DCOLOR
output stage) has to be reproduced in a shader, and each of those reproductions is a place to be
subtly wrong in a way that only shows up on one level, in one lighting condition. A queue-level hook
would have inherited far more of that from the runtime underneath.

The bet was that a total seam with a large reproduction burden is tractable, and a partial seam with
a small one is not, because the partial seam's gaps are unbounded and unenumerable. The
whole-frame residual against the original, 0.13 out of 255, is the evidence that the bet paid, and
it is a *reproduction* claim, which is the only kind of claim a residual settles.

## Open questions

The measurements that justify several structural decisions were taken on level01 and level02, with a
little of level03 and junkyard. The FVF set, the material and stage counts, and every scratch peak are
"measured there" rather than proven for the other campaign levels. The observation that at most two
texture stages are ever active is specifically flagged as worth re-checking before anything depends
on it further.

The vertex arena has no eviction. Slots are freed when a buffer is released, which is correct, but a
level with far more geometry than the ones measured could exhaust it; a counter notices and nothing
recovers.

`vulkan_renderer_plan.md` at the repository root is the authoritative status, and its "Known gaps"
section is longer and more current than this paragraph.

## Where to go next

- [What a residual can and cannot say](/explanation/what-a-residual-can-and-cannot-say/): how a
  change to this renderer is judged, and what the numbers do not mean.
- [How a hook reaches the game](/explanation/how-a-hook-reaches-the-game/): the mechanics of the
  interception itself.
- [How to turn on renderer features](/how-to/modding/turn-on-renderer-features/): what the
  replacement renderer is actually for, from a player's side.
