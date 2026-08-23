---
title: "What a residual can and cannot say"
description: "The same number is a merit figure for a fidelity fix and meaningless for a new feature. Why this project got that backwards, and what it does instead."
weight: 70
audience: ["developer"]
---

This page is for developers changing the renderer, and for anyone reading the measurements in
`vulkan_renderer_notes.md` and trying to work out what they establish. It is about the epistemics of
one instrument, a pixel difference between two frames, and it is the page most likely to change how
you read the rest of the project's numbers.

## The instrument

Capture the same frame twice, under two configurations, and take the mean absolute difference per
channel. The number is quoted out of 255, usually with the percentage of pixels that differ at all:
"0.13/255", "9.56 MAD over 83% of the frame".

The renderer's headline claim uses it. The whole-frame residual of the Vulkan renderer against the
original D3D8 runtime is 0.13 out of 255.

## For reproduction work, the number is the answer

When the goal is to make the new renderer do what the old one did, the residual settles it completely
and smaller is unambiguously better. No judgement is involved, because the target is a specific
frame that
already exists rather than a matter of taste.

The notes are full of these and they are clean claims: `half_pixel` at 1.34/255, `offscreen` at
2.55/255, `viewport_rect` going from 17.23 to 0.089, `shade_mode` at 0.000. Each one says "the
renderer now does what D3D8 did", and the number carries the claim on its own.

This half must not be reframed. Applying the scepticism below to fidelity work would wreck it.

## For a new feature, the number measures reach

Here is the reversal. **A new renderer feature exists to change the look.** A residual against the
baseline therefore cannot say whether the change is any good, and this project's own notes used to
write as though it could.

Every departure once carried a headline of the form "worth 9.56 MAD over 83% of the frame". That
reads as a merit figure. It is the size of a difference, and a difference is not an improvement. A
change that made the game *uglier* would produce the same number, or a larger one.

Numbers are load-bearing everywhere else in that file. This is the one place they are not, and the
failure mode is specific and seductive: a big residual feels like a result.

## What a residual still settles on a departure

Four things, all worth keeping:

**The off path.** `off → on → off` being bit-identical proves the feature is properly gated and that
turning it off restores the shipped renderer. This is a hard invariant, and every departure in the
renderer has it.

**The floor.** Two captures at the *same* setting say what the noise is. Without that, "this changed
something" is a hope rather than a statement. That matters in practice: level02 does not hold
still, and the main menu animates at 1.137 MAD over 5% of the frame all by itself.

**Coverage, and specifically the difference image.** *Where* a feature lands is checkable even though
whether it is good is not. Per-pixel lighting concentrating on units and curved geometry and reading
zero on flat ground is exactly what Gouraud shading cannot represent, and that is
evidence. Its 0.48/255 on its own is not.

**Blast radius, which is the one place a big number is a defect signal.** ACES tone mapping recoloured
99.61% of the main menu, which said that it had reached the 2D half of the game, a half it had no
business touching, rather than that the look was strong. A departure that spreads further than its own
description is wrong regardless of how it looks.

## What actually settles a departure

Looking at it, and playing it. That is what happened every time, and the record is unambiguous:

- Map shadows shipped **off**, because no measurement could say whether the picture with them was
  right. They are on by default now, and what changed it was a play report: the map lights were not
  casting any.
- Two of the renderer notes' sections began as play reports (flames that came and went with
  distance, a ledge "much redder in Vulkan"), and both were real defects that every counter in the
  frame had been clean through.
- The three decisions behind the ambient-occlusion kernel were settled by looking at the output. An
  under-sampled fixed kernel reads as a fan of silhouette outlines rather than as noise. No
  residual would have said so.

## What wrecks a reading

These are recorded because each of them produced a wrong answer first.

**Frame time under FIFO measures the monitor.** Under vertical sync the frame time is quantized to
the refresh interval, so a performance A/B taken there is reading the display. Several sections of
measurement were taken against a vsync ceiling before this was noticed, which is why every profiler
frame now carries a `throttled` flag rather than relying on the measurer to remember.

**A moving frame has no floor worth the name.** Unpaused, level02's animating fires put the noise at
5.44 MAD over a quarter of the frame, larger than most of the effects being measured.

**One blinking string can account for everything.** The `ACTIVE PAUSE` text at the bottom left
accounted for *every* difference across sixteen comparison levels in one bloom study, including two
pairs that should have been identical, and made a bit-identical culling A/B read 0.017 against a
0.007 floor.

**The debug build is not the same software.** For anything timed: level02 runs at 22.5 ms/frame in
Debug against 9.2 ms optimized, and the *ranking* inside a profile changes with it, not only the
scale.

## The general shape of the argument

This page is a specialisation of something the whole project runs on: **a measurement that cannot
come out wrong is not evidence.**

The same instinct shows up in unrelated places. Every test harness here is expected to be broken
once, deliberately, and confirmed to report it. The differential vertex-format harness ships a
`-SelfTest` mode for exactly that, because it has an obvious way to become vacuous in which both
halves resolve to the same symbol and every case passes trivially. The pbr test suites are documented
as *unsafe under pytest*, because their checks append to a list instead of asserting and pytest would
report them green whatever failed.

And it is the same bar that
[Reading a binary that cannot answer back](/explanation/reading-a-binary-that-cannot-answer-back/)
applies to negative conclusions about the game binary. In both cases the danger is a result that
arrives already looking like a result.

## Where this leaves the headline

The 0.13/255 whole-frame figure is a reproduction claim and it is a real one. It says the Vulkan
renderer draws what the original drew. It says nothing at all about HDR, bloom, ambient occlusion,
soft shadows, per-pixel lighting, tessellation or lighting maps, all of which exist to make the game
look *different*, and all of which are one switch away from being off, so that the fidelity claim
stays available alongside them.

A reader who wants to know whether those features are any good will not find the answer in a number,
in this documentation or in the notes. They will find coverage figures, difference images, and
several paragraphs of somebody describing what they saw.

## Where to go next

- [Why the renderer seam is the device](/explanation/why-the-renderer-seam-is-the-device/): what is
  being measured, and against what.
- `vulkan_renderer_plan.md` at the repository root carries the full statement of this rule and the
  current status of every feature it applies to.
- [Compare two renderers on the same frame](/how-to/development/compare-two-renderers/): the
  procedure that produces the number this page is about.
