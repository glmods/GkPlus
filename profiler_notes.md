# The profiler (`src/Profiler`, `src/JsProf`)

Where the frame's CPU time goes, readable from JavaScript. Two sources over one store:
**instrumented zones** for our own code, and a **sampling thread** for everything else —
including gl.exe, which no amount of instrumentation on our side can reach.

It exists because of `vulkan_renderer_notes.md` §4.79, which ended with the frame time
unexplained and one suspect (`ConvertVertices`) explicitly unmeasured. §7 below is what it
measured.

## 1. Why the frame is the unit

Every aggregate here is over a window of recent frames, and there is no session total anywhere.
That is a response to the specific pathology: level02 settled at 5.13 ms on one run, 17.39 on
the next, and drifted 23.2 → 11.4 across five consecutive windows with no knob touched. A
running sum is exactly the instrument that cannot see that. `prof.worst(n)` — the slowest frames
in the ring — is the query a stutter needs, and it is only possible because frames are kept
individually.

The same reasoning gave `render.vulkan_report` its "read these two as a *difference* across a
window" note; this generalises it.

## 2. Two self-checks that are not optional

**`frame.throttled`.** Every frame record carries the present mode and whether it throttles.
§4.79's rule — read the present mode before quoting a frame time — is a field rather than a
discipline, because as a discipline it failed and cost three sections of work. Set by
`prof::NotePresentMode`, called from `CreateSwapchain` where the mode is chosen.

**`prof.overhead_ms`.** A zone costs a calibrated ~60 ns (measured at `Arm` by timing 4096
open/close pairs), and the last frame's event count prices it: 0.012 ms for the ~120 events a
level02 frame records, against a 25 ms frame. This is why `Cat::Draw` is off by default — at
~700 draws a frame it would be 0.04 ms, which is 1% of the frame in the original d3d8 build and
therefore visible in what it is measuring.

## 3. The store

One `Event` per closed zone, 16 bytes:

```
uint64 t_begin | uint32 dur | uint16 site | uint8 depth | uint8 kind
```

Recorded **on close**, not as a begin/end pair — half the events, and the start timestamp is
already in the `Zone` object so no shadow stack is needed. Records therefore arrive in *end*
order, children before parents; every reader sorts on `t_begin` before walking anything.

- **Thread slots** are a static array of 8, claimed by CAS on thread id, with the handle
  duplicated at claim time for the sampler. A slot whose thread has exited is reclaimed
  (`GetExitCodeThread`), which matters because the executor is created at level start and
  destroyed at level end — without reclamation eight level loads would exhaust the table.
- **Rings are allocated on `Arm` and never freed.** A recorder may be inside one on the other
  thread at the moment a disarm is asked for and there is no cheap way to know; a disarm
  publishes a zero mask and leaves the memory. Re-arming with a larger capacity grows and never
  shrinks.
- **Nothing on the recording path allocates or takes a lock.** The one exception is the first
  zone a thread ever opens, which claims its slot under the registry lock.
- The frame boundary is `CaptureDevice::Present` — the one point both renderers pass through,
  and already the place the capture layer counts frames.

`Zone` tests its category against `ActiveMask` **inline**, so a disabled zone is a load, a test
and a not-taken branch. `Zone::End()` closes early for the one case that does not nest in a
block: the Vulkan record pass, which runs from `vkBeginCommandBuffer` to `vkEndCommandBuffer`
inside a much longer function.

### Self time is exact, and the sort that makes it so

A zone's self time is its duration minus its direct children's, computed at read time by sorting
the window by `t_begin` and walking it with a stack. The tie-break is load-bearing:

> On equal `t_begin`, the **enclosing** zone must sort first. QPC ticks at 100 ns here — a few
> hundred instructions — so an outer zone whose first statement opens an inner one lands on the
> same tick routinely. Sorted the other way the stack adopts the parent as its own child's child
> and subtracts the parent's whole duration from the child, which underflows because both are
> unsigned. It read **645636042579833 ms/frame** for `upload/ConvertVertices` the first time this
> ran, which is at least an obvious wrong answer; a shallower version of the same bug would have
> been a plausible one.

The subtraction is clamped as well as sorted for, because a window beginning mid-zone can hold a
child whose parent was trimmed off the front.

## 4. The sampler

A dedicated thread at `THREAD_PRIORITY_TIME_CRITICAL`: `SuspendThread` →
`GetThreadContext(CONTEXT_CONTROL)` → `ResumeThread`, then record. `GetThreadContext` is what
actually blocks until the suspend has landed — `SuspendThread` is asynchronous, so the EIP is
only trustworthy because of that call. **Nothing between the suspend and the resume allocates,
formats or takes a lock the suspended thread could hold**; the sample is written after the
thread is running again.

The clock is a `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` waitable timer, **not** `timeBeginPeriod`:
raising the system timer resolution would change the scheduling of the game's own loop, which is
the thing being measured.

**Read `effective_hz`, not `hz`.** A periodic waitable timer does not hold 1 kHz under this
load — 1000 asked, **714 measured** — and a flat profile only stands for time if the sampling is
uniform.

### The join with the zones

At sample time the sampler also reads the suspended thread's **published current site**, a single
relaxed load out of the thread slot. That is what makes the two sources one profile rather than
two: "of the samples inside `ConvertVertices`, which instructions" is a single query, and
"60% of samples are in no zone at all" is how you find the code that is not instrumented yet.

### Stack walking

Off by default (`GKPLUS_PROFILER=stacks`, or `prof.configure({stacks: true})`), and `prof.stacks()`
is the query. Frames live in their own arena indexed by the sample's ring position, so a sample
costs 16 bytes when this is off and 16 + `4 * stack_depth` when it is on.

**The walk cannot fault, and it had to be built that way rather than guarded.** The obvious
approach — `__try`/`__except` around each dereference — does not work here:

> clang-cl on x86 compiles `__try`/`__except` and then **does not catch the access violation**.
> Verified with a standalone 32-bit test: the valid read returns, the bogus one takes the process
> down with `0xC0000005`. A faulting read inside a suspended-thread window would kill the game
> with its own threads stopped, so this is not a risk worth carrying.

So safety is structural. Every address the walk dereferences lies inside a region `VirtualQuery`
has already reported `MEM_COMMIT` with a readable protection — cached per thread slot as
`stack_low`/`stack_high`, and **refreshed outside the suspend window**, using the previous
sample's ESP, so the syscall is never paid with a game thread stopped. Three further guards are
correctness rather than safety: a frame pointer must be 4-aligned, must climb strictly (a corrupt
chain would otherwise loop), and its return address must land in a loaded image — tested against a
module table rebuilt every 2048 ticks, also outside every suspend, because `EnumProcessModules`
takes the loader lock and taking the loader lock while a thread is suspended inside it is the
classic profiler deadlock.

**gl.exe keeps frame pointers.** This was assumed to be FPO-compiled and it is not: on level02
**92% of samples reach the full depth**, 4.5% are barren, and the chains run from our code
through AWAPI into the game's scene graph and on to `BaseThreadInitThunk`. The hot one is

```
BuildDrawRecord <- Aw_DrawIndexedPrimitiveUP <- SubMesh_DrawIndexed <- SceneNode_Render <- ...
```

`SceneNode_Render` recursing down the scene graph is what saturates the default depth of 12 — the
answer is in the first six frames, but raise `stack_depth` (capped at 32) if the tail is the
question.

### Symbolization

The sampler stores raw addresses; `Describe` resolves them to `module+0xrva` at read time via
`GetModuleHandleEx`. A symbol map turns that into names.

**`utils/symdump/gl_symbols.py`** exports one from the Ghidra database — gl.exe ships no symbols
but its database is heavily named. Last run: **12,487 functions, 7,758 named (62.1%), 977 of them
`::`-qualified, 363 KB.** Unnamed functions are exported too, as `FUN_004a1b30`, which beats a
bare RVA: it gives the enclosing function's boundary and pastes straight into Ghidra's Go To.

Install at `<Gunlok>\gkplus\symbols\gl.exe.sym` (`prof.symbol_dir`). **Nothing has to be loaded by
hand** — the first time `Describe` needs a name for a module it tries `<module>.sym` there once
and remembers a miss, so a machine with no maps pays one failed `fopen` per module per session.

The map's `# file_size` is checked against the module actually loaded, and a mismatch reports
`stale: true` with a note. That is not decoration: a map from a different build shifts every RVA,
so it would produce names that are **confidently wrong rather than absent**, which is worse than
hex. It still loads — being told is the point. An explicit `prof.symbols()` *replaces* whatever
was loaded for that module, including an auto-loaded map.

GkPlus's own `d3d8.dll` has no map and does not need one; we have the PDB:

```
llvm-symbolizer --obj=build/Debug/d3d8.dll --relative-address 0x146ec
```

RVAs move between builds. Record the *names*.

### What was tried and removed: discovering threads the profiler is never called from

A thread becomes visible by *recording* something. A discovery pass was written — register the
executor from the game's own `ExecutingThread` global so the sampler watches it whether or not
it runs instrumented code — and removed, for two independent measured reasons:

- **It found the main thread.** `*(DWORD *)0x007b9d7c` read the id of the thread that presents,
  not an executor, through a whole level02 session. (`src/MakeRole.cpp` uses the same global for
  a *client-or-executor* test, which does not require it to name a second thread; that use is
  unaffected.)
- **Its handle broke the sampler.** A slot whose handle comes from `OpenThread` makes
  `GetThreadContext` return the **stale WOW64 context**: every sample read
  `ntdll!NtWaitForMultipleObjects+0xc`, the 32→64 transition, whatever the thread was actually
  running. The same thread sampled through a `DuplicateHandle`-derived handle gives live EIPs.
  Reproduced four times and A/B'd on one build with an env switch. Both handles are opened with
  `THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT`, so the access mask is not the difference. The
  cause is unexplained; the workaround is to not do it.

The signature is worth recognising: **a flat profile where every sample sits at one ntdll
address is not a game that is waiting, it is a context that was never refreshed.** The zone
attribution stays correct throughout, which is what makes it confusing — the two disagree, and
the zones are the ones telling the truth.

## 5. The JS surface

`prof` in `src/JsGk.cpp`'s `Namespaces` table, typed in `types/gk.d.ts`. `prof.enabled`,
`prof.mask`, `prof.frame`, `prof.frames(n)`, `prof.worst(n)`, `prof.zones(n)`, `prof.samples(n)`,
`prof.threads`, `prof.sampler`, `prof.sites`, `prof.overhead_ms`, plus `mark`/`count`/`scope` for
script-side timing and `trace`/`symbols`/`configure`/`reset`.

`prof.config` is the read-back of the last four: the sizes `configure` is currently running with.
It exists because `configure` **leaves an absent key alone**, so anything that shows a field
before it is edited has only the documented default to show — and that is wrong the moment
`GKPLUS_PROFILER_HZ` or an earlier `configure` has been through. A UI without it displays
`sampler_hz: 1000` over a sampler asked for 250.

### As ImGui: `examples/prof-panel.mjs`

The whole surface as a panel, drawn into the caller's window like `render-panel.mjs`. Two rules
govern it and neither is a style preference:

- **Query on a cadence, and only what is on screen.** `zones`/`samples`/`stacks` each walk a
  window of the ring and build one JS object per row — thousands, for a sampled profile. Running
  those every frame would make the panel the most expensive thing in the frame it is measuring,
  and **`overhead_ms` would not show it**, because that cost is on our side of the binding. So one
  snapshot per section, refreshed on an interval or a button, and a collapsed section queries
  nothing. `prof.frame` is the exception — one object, read every frame, and it is what makes the
  frame graph continuous.
- **Write a setting only when the widget says it changed**, as in `render-panel.mjs`; here because
  `enabled`/`configure` re-arm and `reset` empties the history. The subtle one is the trigger: an
  options object with no `enabled` means *"arm it with these settings"*, so a `min_ms` slider must
  pass the current `enabled` explicitly or dragging it arms a trigger nobody armed.

The graph needed `PlotLines`/`PlotHistogram`, which `imgui-quickjs` did not have — array in,
`values_offset` deliberately not exposed (a ring index that does not match the array it came from
plots the wrong samples silently). A frame index that jumps reseeds the graph from `prof.frames`
rather than plotting the gap as data, which is what makes closing and reopening the overlay
honest.

**Its top of scale is the 95th percentile, not the tallest bar**, and that is the one thing there
worth remembering. Fitting to the max is what such a graph obviously ought to do, and measured on
level02 it is useless: one 126 ms level-load frame against a 30 ms baseline flattens every other
bar into a stripe, so the trace whose shape you opened the panel to read is the only thing you can
no longer see. The outlier clips off the top and `max` stays in the overlay text, which is where a
number belongs anyway.

Armed at boot with `GKPLUS_PROFILER=1` (`=zones` for no sampler, `GKPLUS_PROFILER_HZ` for the
rate), from the first `FrameMark` rather than from `DllMain` — it allocates several megabytes and
starts a thread, neither of which belongs under the loader lock.

`prof.trace(path)` writes a Chrome-trace/Perfetto document: one track per thread, zones as
complete events, samples as instants carrying their caller chain in `args.callers`, and **frames
on their own track labelled with whether they were throttled**. It was built before any ImGui
panel deliberately — a real flame chart across both threads for a fraction of the work.

## 6. Testing it

`utils/rendertest`'s harness plus the REPL, on level02 as usual:

```powershell
. .\utils\rendertest\shoot-settled.ps1
$env:GKPLUS_PROFILER = "1"
Start-Gunlok -Renderer vulkan
Repl 'levels.start({script: "level02.gls", console: "level02.gcs"})'
Dismiss-Briefing; Wait-World
Repl 'prof.reset()'          # then let it run; every window is relative to the reset
Repl 'JSON.stringify(prof.zones(120).map(z=>[z.name,+z.self_ms_per_frame.toFixed(3)]))'
```

Reset **rebases the sampler's tick count too**, and its walk counters. It did not at first, which
made `taken` and `ticks` incomparable and the sampler look like it was missing 80% of its samples
when it was not — then the identical bug reappeared with `walks`/`barren` when stack walking was
added, reading 33,878 walks against 8,978 samples. A counter that survives a reset next to one
that does not is a lie by arithmetic; check both halves when adding one.

## 7. What it found on the first run

level02, settled, `GKPLUS_RENDERER=vulkan`, 120-frame window, **FIFO — so the frame time is the
monitor's**. The frame is 25.2 ms and `frame.throttled` says so.

| zone | self ms/frame | calls/frame | worst call |
|---|---|---|---|
| `upload/ConvertVertices` | 4.09 | 33 | 0.484 ms |
| `upload/UploadIntoSlot` | 3.69 | 33 | 0.391 ms |
| `vulkan/record` | 0.835 | 1 | 0.920 ms |
| `upload/UploadLocked` | 0.668 | 38 | 0.851 ms |
| `vulkan/present` | 0.147 | 1 | 0.341 ms |
| `Present` | 0.131 | 1 | 1.624 ms |
| `vulkan/acquire` | 0.124 | 1 | 0.272 ms |
| `vulkan/submit` | 0.038 | 1 | 0.078 ms |
| `vulkan/DrawFrame` | 0.016 | 1 | 1.470 ms |
| `vulkan/fence wait` | 0.005 | 1 | 0.008 ms |

**The upload path is 8.4 ms of a 25.2 ms frame** — §4.79's named suspect, now measured. The whole
Vulkan submit path is ~1.2 ms and `fence wait` is 0.005 ms, so under FIFO the renderer is not
where the wait lands either.

The sampled profile (2170 samples, 714 Hz effective) adds what the zones cannot see. **60% of
samples fall outside every zone**, and symbolized they are:

| | share | |
|---|---|---|
| `d3d8::BuildDrawRecord` | 11.8% | `D3D8Capture.cpp:2416` |
| `d3d8::ResolveLightRun` | 6.5% | `D3D8Capture.cpp:2267` |
| `d3d8::CaptureDevice::EmitDrawUP` | 4.8% | `D3D8Capture.cpp:2723` |

Inside the instrumented zones the leaves are `vulkan::ReadFloat` (`VertexFormat.cpp:26`),
`vulkan::NoteVertexBounds` (`VkResources.cpp:201`) and `std::max<float>`.

So roughly **55% of main-thread samples are in GkPlus's own capture and upload layer**, split
between the upload path that was already suspected and a per-draw record path
(`BuildDrawRecord` + `ResolveLightRun` + `EmitDrawUP` ≈ 22%) that was not on anyone's list. Those
three run per draw inside `EmitDraw`, outside any zone — which is exactly the gap the sampler
exists to cover.

Stack walking then named the caller, which the flat profile could not: the 26.5% plurality stack
in the window is

```
BuildDrawRecord <- Aw_DrawIndexedPrimitiveUP <- SubMesh_DrawIndexed <- SceneNode_Render <- ...
```

`SceneNode_Render` recursing down the scene graph, into `SubMesh_DrawIndexed`, into AWAPI's
`Aw_DrawIndexedPrimitiveUP`, into our capture of it. `rendering_notes.md` §5 already said the
single biggest producer is the `Unit` hierarchy's slot 68; this is the same story measured from
the other end, and it says the cost is **per draw call on the game's own submit path**, not
per frame and not in the renderer.

**None of this is a claim about the original d3d8 build**, which these zones do not instrument
and which the same session measured at a steady 4.25 ms.

## 8. Catching a stutter: windows and the trigger

The three rings have very different reach, and that asymmetry is the whole problem:

| ring | 262144 entries costs | covers at 40 fps | at 200 fps (unthrottled) |
|---|---|---|---|
| frames (48 B) | 2.6 MB for **65536** | ~27 min | ~5.5 min |
| samples (16 B + 4×depth) | 17 MB | ~6 min | ~6 min |
| events (16 B **× 8 slots**) | 32 MB | ~55 s | **~11 s** |

So `prof.worst(20)` finds a stutter from twenty minutes ago and its samples may well still be
there, but **its zones are long gone** — and events get scarcer the faster the game runs, which
is the opposite of what you want, since unthrottling is the first thing stutter-hunting requires.

**Unthrottle first, always.** Under FIFO the frame time is quantized to the refresh interval: a
6 ms hitch is absorbed entirely and a 20 ms one reads as one extra interval. Both the threshold
and the baseline would be measuring the monitor. `GKPLUS_VK_PRESENT_MODE=immediate`. This is
§4.79's lesson one level up, and `frame.throttled` plus `capture.throttled` are there so it
cannot be forgotten.

### Windows

Every read takes a `Window`, and there are three ways to name one:

```js
prof.zones(120)                                  // the last 120 frames
prof.zones({around: 5117, pre: 60, post: 20})    // a frame that has already scrolled past
prof.zones({capture: 0})                         // a window the trigger saved
```

The second did not exist and is why this section was written: a stutter could be *seen* in
`prof.worst()` and not profiled, because "the last N frames" was the only window expressible.
Its samples were sitting in a ring covering six minutes and there was no way to ask about them.

### The trigger

`prof.trigger = {min_ms: 12, multiple: 3, pre: 90, post: 30}` arms the flight recorder. At each
`FrameMark` a frame that exceeds **both** an absolute floor and a multiple of the running median
schedules a snapshot; the snapshot is taken `post` frames later and copies the surrounding window
out of the rings into storage they cannot reach.

Four things it does deliberately:

- **A median, not a mean.** A mean is dragged up by the very frames the trigger exists to notice,
  so after a few stutters it stops firing on them. 64 frames, `nth_element` over a stack array,
  no allocation.
- **Both conditions.** A floor alone is wrong for every scene — a menu frame and a level frame
  differ by 4x before anything goes wrong. A multiple alone trips on ordinary jitter in a fast
  steady game.
- **`post` is not padding.** A stutter's cause often shows in the recovery — a ring wrap, a
  reload, a re-bake — so the snapshot is deferred rather than taken the instant the trigger fires.
  A second stutter inside the post window does not restart the countdown, or a run of them would
  never capture anything at all.
- **Every destination is reserved at `Arm`.** A capture is memcpy and never allocation: it runs
  on the main thread inside Present, immediately after a frame that already stuttered, and a
  512 KB malloc there would be a second stutter that reads as part of the first one's cause. The
  cost of that is a fixed capacity, so a capture that overflows says `truncated: true` rather
  than silently holding part of the window.

Measured on level02 → level03 unthrottled, with `{min_ms: 12, multiple: 3, pre: 90, post: 30}`:
idling produced no captures at all, and the level load produced four, from 32 ms to 1536 ms
against a baseline of 2.1–12.9 ms. The 1536 ms one hit the event cap and said so. Read back with
`prof.stacks({capture: n})`, the top stacks are the answer directly:

```
LoadOrBuildSectionAdjacency+0x273  <-  ParsedThingBase::ToMap+0x2587      16.6%
gameoverlayrenderer.dll+0x000928a9                                        25.0%
```

— the level build, named by the symbol map, and Steam's overlay.

### Three JS-binding bugs this turned up, all the same mistake

`GetInt32Prop`'s `bool` is **"no exception pending"**, not "the property was there" — it leaves
its out-param untouched for an absent property, which is what makes a pre-seeded default work.
Read as presence it caused, in three separate places:

- every object window resolving to `{capture: 0}`, including one that said `{around: ...}`;
- `prof.trigger = {min_ms: 30}` zeroing `pre` and `post`;
- `prof.configure({frames: 100})` setting `events_per_thread`, `stack_depth` and every other
  numeric field to 100 as well, because they shared one `value` variable.

The pattern that is correct: seed the out-param with the value already in force, and check the
return **only** for an exception. It is now a table in `Configure` for exactly that reason.

### And one in the sampler

`StartSampler` is a no-op once its thread exists, so a re-arm that changed `sampler_hz` reported
the new rate and went on sampling at the old one. `Arm` now stops a running sampler whose rate
has changed. Verified: 400 asked, 370 effective — and note that shortfall is 8% where 1000 asked
gives 714 (29%), which says the loss is a fixed cost per tick rather than a proportional one.

## 9. What is not built

- **GPU timestamps.** `VK_QUERY_TYPE_TIMESTAMP` at pass boundaries, read back
  `kFramesInFlight` later so nothing waits. ~150 lines, and it would have settled §4.76–4.79
  without a single A/B. Deferred because the evidence says the GPU is not this scene's cost.
- **An ImGui panel.** `prof.trace` into Perfetto covers it for now — and the trace now carries
  each sample's caller chain in its `args`, so the flame data is there without one.
- **A `d3d8.dll` symbol map.** Deliberate: the PDB plus `llvm-symbolizer` answers it offline, and
  RVAs move every build so a checked-in map would be stale immediately. If reading our own frames
  in-game becomes the common case, a linker `/MAP` converted to the `.sym` format is the cheap
  route, not a second Ghidra database.
- **Checking `gl.exe.sym` into the repo.** It is generated, 363 KB, and derived from a database
  that is not in the repo — so anyone else has to generate it. Worth reconsidering.
- **The executor thread has never been observed recording anything.** It reaches
  `BufferWrapper::UploadLocked` per §4.72, which is instrumented, but no level02 session so far
  has produced a second thread slot. That is a statement about what has been measured, not a
  claim that it never does — and after the discovery experiment above, the profiler will not
  invent a thread it has not been called from.
