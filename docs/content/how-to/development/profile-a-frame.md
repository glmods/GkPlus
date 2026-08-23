---
title: "Profile a frame"
description: "Find where a frame's CPU time goes with instrumented zones and the sampler, and catch a stutter you cannot reproduce on demand."
weight: 80
audience: ["developer"]
---

This guide shows a **developer** how to measure where CPU time goes inside Gunlok.

## Set the run up so the numbers mean something

Three things before you launch, all of which have invalidated real measurements here:

1. **Deploy RelWithDebInfo.** level02 runs at 22.5 ms/frame under Debug against 9.2 ms optimized,
   and the ranking *inside* the profile changes with it, not just the scale. See
   [Build and deploy an optimized DLL](/how-to/development/build-an-optimized-dll/).
2. **Unthrottle.** Under FIFO the frame time is quantized to the refresh interval, so both a
   threshold and a baseline end up measuring the monitor. Set
   `GKPLUS_VK_PRESENT_MODE=immediate`. Every frame record carries `throttled` so this cannot be
   forgotten silently.
3. **Install the symbol map**, or every sampled stack reads as hex. Export it from the Ghidra
   database and drop it in the profile:

   ```
   analyzeHeadless <project dir> <project> -process gl.exe -noanalysis \
       -scriptPath utils/symdump -postScript gl_symbols.py <output path>
   ```

   Install at `<profile>\symbols\gl.exe.sym`, where it is found with no call; `prof.symbol_dir`
   reports the path. `# file_size` is checked against the loaded module and `prof.symbols()` reports
   `stale: true` on a mismatch, since a map from another build shifts every RVA and is confidently
   wrong
   rather than absent.

## Arm it and reach a level

```powershell
$env:GKPLUS_PROFILER = "1"                     # "=zones" for no sampler
$env:GKPLUS_PROFILER_HZ = "1000"               # optional; the sampler rate
$env:GKPLUS_VK_PRESENT_MODE = "immediate"
. .\utils\rendertest\shoot-settled.ps1
Start-Gunlok -Renderer vulkan
Repl 'levels.start({script: "level02.gls", console: "level02.gcs"})'
Dismiss-Briefing; Wait-World
```

Use **level02**, not level01: `level01.gcs` ends in `PLAY CUTSCENE first contact`, so a scripted run
lands in a camera sequence whose progress depends on how fast the machine got there.

The profiler is armed from the first frame mark and not from `DllMain`, because it allocates
several megabytes and starts a thread.

## Read it

Every window is relative to the last reset, so reset once the scene is the one you want to measure:

```powershell
Repl 'prof.reset()'
Repl 'JSON.stringify(prof.zones(120).map(z => [z.name, +z.self_ms_per_frame.toFixed(3)]))'
```

Then, as needed:

| what you want | call |
|---|---|
| this frame, every frame | `prof.frame` |
| the last N frames | `prof.frames(n)` |
| the worst N frames in the ring | `prof.worst(n)` |
| instrumented zones over a window | `prof.zones(window)` |
| the sampled profile | `prof.samples(window)` |
| sampled call chains | `prof.stacks(window)` |
| what is instrumented at all | `prof.sites` |
| the sizes actually in force | `prof.config` |

Read `prof.config` rather than assuming the defaults: `configure` leaves an absent key alone, so a
panel that shows `sampler_hz: 1000` over a sampler asked for 250 is the failure it exists to
prevent.

Two self-checks are not optional. **`throttled`** says the frame time measured the monitor.
**`overhead_ms`** is the profiler's own cost, but it cannot see time spent on *our* side of a
binding, so a script querying `zones`/`samples`/`stacks` every frame is a cost it will not report.

For a panel instead of a socket, install `examples/prof-panel.mjs`. Its two rules are worth
copying: snapshot on a cadence and query nothing from a collapsed section, and pass the current
value explicitly on every trigger write, because an options object with no `enabled` *arms* it.

## Instrument something new

One macro, in `src/Profiler.h`:

```cpp
GK_ZONE("UploadStaging", Cat::Upload);
GK_ZONE_FN(Cat::Render);                 // names the zone after the enclosing function
```

`name` must be a literal or a pointer that outlives the process, since the site table stores it rather
than copying. Pick the category from `Cat` (`Frame`, `Render`, `Upload`, `Script`, `Io`, `Game`,
`Draw`); `Draw` is per-draw and off by default. A disabled zone is a load, a test and a not-taken
branch, so zones are always compiled.

`Zone::End()` ends one early where the thing being timed does not nest in a block, and is
idempotent. For something with no extent use `prof::Instant`; for a sampled number use
`prof::Counter`.

Adjust what is recorded with `prof.mask`, and the ring sizes with `prof.configure`.

## Catch a stutter you cannot reproduce on demand

The three rings have very different reach: unthrottled the frame ring covers minutes and the event
ring around eleven seconds, and events get *scarcer* the faster the game runs. So `prof.worst(20)`
will find a stutter from twenty minutes ago and its zones will be long gone. Arm the flight recorder
instead:

```powershell
Repl 'prof.trigger = {min_ms: 12, multiple: 3, pre: 90, post: 30}'
```

A frame that exceeds **both** an absolute floor and a multiple of the running median schedules a
snapshot, taken `post` frames later, copied out of the rings into storage they cannot reach. Read it
back by naming the capture as the window:

```powershell
Repl 'JSON.stringify(prof.captures)'
Repl 'JSON.stringify(prof.stacks({capture: 0}).slice(0, 10))'
```

A capture that overflowed its reserved space says `truncated: true` instead of silently holding
part of the window. `prof.clear_captures()` empties them.

There are three ways to name a window, and the second is what makes a frame that has already
scrolled past readable at all:

```js
prof.zones(120)                                  // the last 120 frames
prof.zones({around: 5117, pre: 60, post: 20})    // a frame that has scrolled past
prof.zones({capture: 0})                         // a window the trigger saved
```

## Take it out of the game

```powershell
Repl 'prof.trace("D:\\traces\\level02.json")'
```

writes a Chrome-trace/Perfetto document: one track per thread, zones as complete events, samples as
instants carrying their caller chain, and frames on their own track labelled with whether they were
throttled. That is a real flame chart across both game threads for a fraction of the work of reading
tables over the REPL.

## Related

- `profiler_notes.md`: the design record; §7 is the first run's findings and §8 the ring
  arithmetic that says when you need a trigger.
- [Compare two renderers on the same frame](/how-to/development/compare-two-renderers/): for a
  difference in the *picture* rather than in the time.

## Reference and background

- [Environment variables](/reference/data/environment-variables/): `GKPLUS_PROFILER`, the
  present-mode switch that has to be set before any frame time means anything, and the rest.
- [`prof`](/api/js/variables/gk.gk.prof.html): every member of the profiler namespace, in
  the generated JavaScript reference.
- [Command-line utilities](/reference/data/cli-utilities/): `symdump`, which is what makes
  a sampled stack read as names instead of hex.
