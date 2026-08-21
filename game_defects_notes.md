# Game defects (Gunlok's own, not GkPlus's)

Bugs that live in `gl.exe` and reproduce without GkPlus. Recorded here so a later
session can decide whether the mod should paper over them, and so nobody spends a
second evening blaming our hooks for them.

Other files cite these **by number** (`game_defects_notes.md` §4), so a number is a
stable id: append a new defect at the end of the numbered run — before *Debugging
Gunlok*, which is an appendix and not a defect — and never renumber an existing one.

| | Defect | Reachable from |
|---|---|---|
| 1 | `Font_QueueText` smashes its stack past ~1024 characters | the game itself (training debrief) |
| 2 | `PrintParseWarning` / `PrintParseError` discard their output | — (a trap, not a bug) |
| 3 | `GetResourceString` ignores its table's terminator | a missing localized string id |
| 4 | the process faults on exit via the console `QUIT` | any exit |
| 5 | the section adjacency test overflows its shared-vertex buffer | degenerate-after-weld level geometry |
| 6 | `TexMergePolys` writes a UV list before deciding not to merge | an authored `SHPMRGDT` |
| 7 | `Role::interface_beam_script` is shared by pointer and freed per actor | GkPlus only (`make.role`) |
| 8 | `Font_QueueText` dereferences null when its node allocation fails | out-of-memory |
| 9 | `ToRole` dereferences a null `Hierarchy` for a missing `.rif` | a role whose `.rif` is absent |
| 10 | ambient sound volume (`V` in a `DUMOBJTX`) is parsed and discarded | every level in the game |
| 11 | the pool allocator's critical section is gated on a flag nothing sets | every allocation |
| 12 | the HUD's meters and item icons are drawn under the world's depth slice | every frame of every level |
| 13 | three unchecked writes reachable from any peer in a multiplayer session | the wire (commands `0x2a`, `0x2d`, `0x28`/`0x8d`) |
| 14 | `InventoryInfo_Ctor` leaves `pickup_radius` uninitialised on both paths | every role with an inventory |
| 15 | `SoundSample_ReadWholeFile` leaks the file `HANDLE` when the read fails | a truncated or unreadable sound file |
| 16 | `AiBeginInvestigate` builds its deadline with no clock read (PROPOSED) | a minebot spotting something |
| 17 | the in-game pause menu's Options / Load / Save cannot be opened (PROPOSED; live check INCONCLUSIVE, §17.3) | the pause menu, every level |

---

## 1. `Font_QueueText` @ 0x005782e0 smashes its stack on any string over ~1024 chars

**Status:** open, unpatched. Reproduces reliably. Blocks the training-level debrief.

**Severity:** fatal — overwrites the return address, so it is an arbitrary stack
smash, not a graceful failure.

### Mechanism

`Font_QueueText` is `__thiscall` with the text as its second stack argument
(`Stack[0x8]`, i.e. `[EBP+0xc]`; the first, `[EBP+8]`, is the `RECTF *`). Its
prologue is `SUB ESP,0x478` and it copies the caller's string into a **1028-byte
buffer at `EBP-0x404`** with **no bound**. It then inline-`strlen`s that buffer
into EBX (0x00578324..0x0057833b) and loops `[0, EBX)` replacing glyph-less
characters with `'?'` (`MOV byte ptr [EBP+EDI*1-0x404],0x3f` @ 0x0057838e).

1028 bytes below EBP means the copy runs into the saved EBP at `+0`, the return
address at `+4`, and the arguments at `+8` once the string passes 1028/1032/1036
characters.

The observed crash is the *next* instruction reloading the now-clobbered argument:

```
005783a1  MOV   EAX, dword ptr [EBP + 0x8]      ; arg 1, overwritten by the text
005783a4  MOVSS XMM0, dword ptr [EAX]           ; <-- AV, eax=0x7375202c
```

`0x7375202c` is ASCII `", us"` — a fragment of the text itself. Note the `'?'`
loop is *incidental*: it only stores for characters with no glyph, which is why
the frame holds readable text rather than `0x3f3f3f3f`.

### Reproduction

Complete the Training Level and enter the debrief. The training-completion text is
**1925 characters** (EBX = 0x785), 897 past the buffer:

> "Well done. You have now completed your training. Now here's some advice for the
> real thing.....Focus on your mission...You are a small group against a huge enemy
> force. …"

### Why it is not GkPlus

Established under cdb (`sxe av`) with the envelope build deployed:

- the buffer holds the game's own debrief prose, complete and unmangled — no JSON,
  no envelope, no truncation artifacts;
- `ConsoleCommandLine` @ 0x007b6958 is **empty** at the fault, so no queued console
  command was mid-dispatch — the fault is in the debrief screen's ordinary
  per-frame render of `BriefingTextList` @ 0x007b68bc;
- the smashed frame contains the text (`ret = 0x73656e69` `"ines"`,
  `arg = 0x7375202c` `", us"`), so the *copy* overflowed. Nothing GkPlus does
  bounds or supplies that string.

The arithmetic is build-independent: 1925 characters into 1028 bytes overflows in
vanilla Gunlok too.

### If we decide to fix it

Hook `Font_QueueText` and clamp the text before calling the original — the bound is
`0x404` minus the NUL, and the string is just `Stack[0x8]`. A wrapper that copies
at most 1027 chars into its own buffer and forwards that is ~15 lines and cannot
regress anything, since longer strings currently corrupt the stack rather than
rendering. Wrapping onto multiple lines would be nicer but needs the font metrics
the function already computes.

Deliberately **not** done yet: it is outside the change that found it, and the mod
has no other "fix the game's bugs" hooks to be consistent with.

### Ghidra

`Font_QueueText` @ 0x005782e0 has a plate comment recording all of the above, kept
verbatim as a trailing section under the full analysis of the function. The name was
`DrawText?` while only the buffer handling had been read; the body has since been read
end to end, and the function **draws nothing** — it enqueues a `TextDrawItem` node for
a later flush. That does not change this defect: the unbounded copy is in the layout
pass, before anything is queued. See `rendering_notes.md` for the queue→flush chain and
the `TextFlags` bits.

---

## 2. `PrintParseWarning` / `PrintParseError` discard their output (not a bug, a trap)

`PrintParseWarning` @ 0x00477050 and `PrintParseError` @ 0x00477000 `vsprintf` into
a 2048-byte **local** and throw it away, incrementing counters at 0x00739a3c /
0x00739a38. The output call was compiled out of the shipped build.

So GkPlus's `DebugSystem` hook is the only thing that ever made those messages
visible — and unhooking one routes nothing to the console, it just makes it silent
again. Worth knowing before anyone "restores" them: the GLS parser calls the
warning variant **once per unset field per section**, 13,000+ for one Training
Level load, and redirecting those to `OutputDebugString` makes the game unplayable
under any debugger (each call is a synchronous round-trip). That is why
`RedirectWarnings` is false in `src/Debug.cpp`.

---

## 3. `GetResourceString` @ 0x00579000 ignores its table's terminator

The localized string lookup is a five-instruction linear scan with **no end check** — although,
and this is the correction that changes what a fix would look like, **the table does have a
terminator; this function simply never tests it**. `ResourceStringEntry` is 0x14 bytes and its
`+0x10` field is `is_last`: `LoadResourceStringTable` @ 0x00578f30 sets it with
`SETZ AL` on `(id == last_id)` (0x00578f97 / 0x00578fa3), so exactly the final entry carries it,
and `FreeResourceStringTable` @ 0x00579020 **honours it** — `XOR EAX,EAX / CMP [ESI],EAX /
CMOVZ EAX,EDI` at 0x00579046-0x0057904a continues only while it is zero. So the sentinel is
written by the loader and read by the deallocator, and skipped by the one function that walks the
array looking for something. The fix is a **one-instruction test inside the loop**, not a new
sentinel; an earlier revision of this section said "no terminator", which pointed at the wrong
one.

The scan itself:

```
00579000  MOV EAX,dword ptr [ECX]        ; the table head
00579002  CMP dword ptr [EAX],EDX        ; is this the id?
00579004  JZ  0x0057900d
00579006  ADD EAX,0x14                   ; next 0x14-byte entry
00579009  CMP dword ptr [EAX],EDX        ; <-- faults here; entry->is_last (+0x10) is never read
0057900b  JNZ 0x00579006                 ; ... forever
0057900d  MOV ECX,dword ptr [EAX + 0x4]
00579010  TEST ECX,ECX
00579012  MOV EAX,0x7c14b4               ; a default string when the entry is null
00579017  CMOVNZ EAX,ECX
```

The parameter is `ResourceStringEntry **` — **one level of indirection**, ECX being the *address
of* the pointer global `LocalizedStrings` @ 0x00725664 rather than the array. Census: all 854 call
references reach this function with `MOV ECX,0x725664` and none dereferences first. The contrast
that proves it is deliberate is `FreeResourceStringTable`, which is handed the **array base** via
`MOV ECX,[0x00725664]` @ 0x0046aa72 — the binary distinguishes the two shapes and gives each
function the one it wants.

Ask for an id the table does not hold and the loop runs off the end of the resource
image and dereferences unmapped memory: **`0xc0000005` at fault offset `0x00179009`**
(RVA; the address is 0x00579009, and both the faulting module and the application are
`gl.exe`). Note the function *does* handle a **null** entry — that is what the
0x7c14b4 default is for — so the missing case it does not handle is a **absent** id,
not an empty one.

Observed twice while testing `.RIM` writing, on exit both times, in two separate
runs. It is not GkPlus's: one of the two runs had served no modded file at all, and
the reproduction is a stock code path with ~400 call sites (`SetupMenus`,
`RegisterAllKeyBindings`, `OnMenuItemClicked`, `LoadLevel`, most console commands).
It has not been narrowed to a specific caller or string id — the evidence is the
fault address plus the fact that it fired with and without mods present.

Practical consequence: **a Gunlok crash whose faulting offset is `0x00179009` is a
missing localized string, not whatever you were testing.** Worth checking first,
because it is reached from almost everywhere and it presents as a plain access
violation in `gl.exe` with no other clue. Likely more reachable on a non-English
install, where `glres<lang>.dll` may hold a different set of ids (see the localized
command-name hazard in `console_command_notes.md`).

### A deterministic repro: `MAIN MENU` from inside a level

**Measured 2026-08-19**, in the same run as §17.3's live check. With level02 up —
started over the REPL with
`levels.start({script: "level02.gls", console: "level02.gcs"})`, **178 actors**, camera
settled — `screen.main_menu()`, i.e. the console **`MAIN MENU`** command, crashes the
process immediately. WER `APPCRASH`, faulting application **and** faulting module both
`gl.exe`, exception code **0xc0000005**, fault offset **0x00179009** — this section's own
signature (gl.exe's default 0x00400000 image base, so address 0x00579009).

What it contributes is that it is the first repro here that is neither opportunistic nor
on exit: it is scriptable, it fires from a **known live state** rather than during
teardown, and for *this* instance it narrows the caller from "~400 call sites" to the
`MAIN MENU` console path. That path's command *name* is itself a localized string —
GkPlus reaches it as `GK_LOCALIZED_COMMAND(ScreenMainMenu, 10002)` in
`src/JsCommands.cpp`, and the game's own `SetupConsoleCommands` registers the same
command through `GetResourceString` (it is the `MENU` entry in the list of fifteen
resource-named commands in `console_command_notes.md` §1) — so a missing id somewhere on
that path is the natural suspect, which ties this repro to the localized-command-name
hazard cited just above. Id 10002 itself did resolve: `RunLocalizedCommandImpl` throws a
JS error instead of dispatching when its name resource is absent, so the fault is
**downstream of the name lookup**, in whatever the return to the front end asks for next.

**What this does not establish, plainly: the specific string id was not identified, and
no dump was symbolized. It is a repro, not a diagnosis.** The paragraphs above stand
exactly as written — ~400 call sites, not narrowed to a caller *in general*, and the
evidence that it is not GkPlus's is still the fault address plus its firing with and
without mods. One narrowed path does not settle the general case.

It is also a **worked instance of the practical consequence above**, which is why it is
worth a sentence: the session that measured this crash first filed it under §4 as a
second fault on the teardown/exit path and reasoned from that before checking the offset
against this section. The offset was the whole answer, and it was already written down
here.

---

## 4. The process faults on exit via the console `QUIT`

**Status:** open, undiagnosed. Reproduces every time. Harmless to the player — the
process is leaving anyway — but it means **process-exit cleanup cannot be relied on**.

Sending `QUIT` from the console (or `console.execute("QUIT")` from the REPL) exits
the process *and* leaves a WER minidump in `%LOCALAPPDATA%\CrashDumps`. Observed on
four consecutive runs; a `TerminateProcess` (Task Manager, `Stop-Process`) leaves no
dump, so it is the ordinary exit path that faults, not the kill.

**It is not GkPlus's mod loader**, and that is an A/B measurement rather than an
assumption: the build from before `FileHookSystem` existed produces a dump on the
same `QUIT` (`gl.exe.35284.dmp`). Whether it is *some other* part of GkPlus or
vanilla Gunlok has not been established — testing that needs a run with the mod
removed entirely, which nobody has done.

### The consequence that matters

Anything that tidies up in `DllMain(DLL_PROCESS_DETACH)` is best-effort only, and
that is compounded by `entry.cpp` destroying the `Subsystems` aggregate in reverse
declaration order — a fault in an earlier destructor takes out every later one.
`vfs::Shutdown()` removing its `%TEMP%\gkplus-vfs-<pid>` tree was written that way
and never ran; the working mechanism is the startup sweep in `Vfs.cpp`, which
removes any such directory whose pid is no longer alive. Prefer that shape — *clean
up other people's leftovers on the way in* — over trusting the way out.

**The detach handler itself does run on this exit, though — the fault is later.**
Measured, and worth knowing before writing off the way out entirely:
`settings::SaveIfDirty()` is the *first* statement of the `DLL_PROCESS_DETACH`
branch, ahead of the `Subsystems` teardown, and a value written from the REPL a
moment before a `console.execute("QUIT")` reaches the file. The measurement had to
isolate it — `src/Settings`'s per-frame autosave would have written the same value
for a different reason, so it was disabled for the run (both its thresholds raised
to ten minutes) and confirmed absent from the file after five seconds before the
QUIT was sent. So the rule is about *ordering* rather than about the handler being
unreachable: work placed before the first destructor completes, and anything after
a destructor that faults does not. `vfs::Shutdown()` ran from a destructor
mid-aggregate, which is exactly the position that loses.

### Where to start if someone picks this up

WER already wrote the dump; `cdb -z <dump> -c ".ecxr; k 40; q"` plus
`llvm-symbolizer` on the `d3d8+0x...` RVAs is the recipe below. The dumps from the
session that found this are gone (WER keeps a bounded ring), so reproduce first.

---

## 5. The section adjacency test overflows its shared-vertex buffer on degenerate level geometry

Two implementations, one defect: `PolygonAdjacencyTest` @ 0x0048ecf0 (`Vec3[3]`) and
`NavQuad_AdjacencyTest` @ 0x0048f580 (`Vec3[4]`).

**Status:** open, unpatched in the game; **worked around in the Blender exporter**
(`shapes.welds_degenerate`), which drops the geometry that triggers it.

**Severity:** fatal, and *diagnostically* nasty — it is a `/GS` fast-fail, so
running under a debugger suppresses the WER dump and the fault names neither the
asset nor the polygon.

### Mechanism

Slot 0x50 is `__thiscall(this, other)`, "do these two polygons share an edge?". It
walks `this`'s vertex pointers, scans `other`'s vertex-pointer array for each, and
appends every match — **by pointer identity** — into a fixed local buffer:

```c
_eh_vector_constructor_iterator_(&shared, 0xc, N, ...);   // N x 12 bytes
...
dst = (undefined4 *)((int)&shared + count * 0xc);         // no bound on `count`
if (v == other_verts[i]) { count++; dst[0] = ...; dst[1] = ...; dst[2] = ...; }
```

It returns true when exactly **2** matched — a shared edge — after which it projects
the edge and validates the connection.

There is no capacity check, and in **both** implementations the buffer ends exactly
on the function's `/GS` cookie, so the first overflowing write lands its first dword
on the cookie rather than somewhere survivable:

| | `PolygonAdjacencyTest` @ 0x0048ecf0 | `NavQuad_AdjacencyTest` @ 0x0048f580 |
|---|---|---|
| buffer | `Vec3[3]` at `EBP-0x38` | `Vec3[4]` at `EBP-0x44` |
| cookie | `EBP-0x14` | `EBP-0x14` |
| own vertices walked | 3 | 4 |
| arithmetic | `-0x38 + 3*0xc = -0x14` | `-0x44 + 4*0xc = -0x14` |
| overflows on the | **4th** match | **5th** match |

The epilogue's `__security_check_cookie` then fast-fails with
`STATUS_STACK_BUFFER_OVERRUN` (**0xc0000409**, subcode 0x2
`FAST_FAIL_STACK_COOKIE_CHECK_FAILURE`) — *not* an access violation.

**Both implementations are live.** `SHPMRGDT` fuses triangle pairs into `NavQuad`s
before the sections are built (`rif_chunk_format.md`, "Merging polygons into
quads"), so a level's sections are a mix: **59% quads** across the shipped set —
325,598 against 224,116 triangles — though it varies, and level01 is 47.6%.
`BuildPolygonAdjacencyGrid` dispatches through the vtable
(`(**(code **)(*poly + 0x50))(other)`), so all four tri/quad combinations occur.

### What produces an overflow

Matches are `sum over this's corners of (occurrences of that corner in other)` — a
nested loop, so the ceiling is `|this| * |other|`, not `|other|`. That is how the
observed peak of **9** arises: `this` and `other` both welded down to a single
distinct position, 3 corners x 3 slots.

`other`'s slot count is **read through vtable slot 0x18**
(`iVar7 = (**(code **)(*other + 0x18))()`), not assumed — 3 against a `NavPolygon`,
4 against a `NavQuad`. Two cases follow:

- With `this` **non-degenerate**, each of its distinct corners claims a given slot of
  `other` at most once, so the total is bounded by `|other|`: 3 against a triangle,
  **4 against a quad**. So a perfectly well-formed triangle can still reach its 4th
  match, and hence its cookie, when tested against a **degenerate quad** — which
  needs all four of the quad's slots to hold pointers drawn from the triangle's three
  corners, and so forces a repeat. (`MergePolys` rejects `vert_ind[3] ==
  vert_ind[0]`, so such a repeat is either one of the other pairings or created by
  the weld.)
- With `this` **degenerate** the product bound applies and the count runs away fast —
  the two-degenerate-triangles case in the reproduction below.

Either way the trigger is the same: geometry that is degenerate once the loader has
welded vertex records by position, meeting in the same section-grid cell. A triangle
becomes degenerate at the weld when two of its corners land on the same position, and
`SHPRAWVT` is **integer** — so this includes corners that were distinct in the
authoring tool and collapsed on quantization, which is how an exporter creates them
with no modelling error anywhere.

### Call path

```
ToMap+0x2587
  LoadOrBuildSectionAdjacency+0x307        @ 0x0044fef0 — the .cut sidecar builder
    BuildPolygonAdjacencyGrid+0x137        @ 0x0048aa00 — triple-nested grid loop
      <slot 0x50 on the section>           — PolygonAdjacencyTest @ 0x0048ecf0
                                             or NavQuad_AdjacencyTest @ 0x0048f580
        __security_check_cookie → __report_gsfailure
```

`BuildPolygonAdjacencyGrid` walks the level's 3D section grid (dimensions at
`+0x6c`/`+0x68`/`+0x64`, cells at `+0x34`), and for each section rescans the 26
neighbouring cells, calling slot 0x50 on each candidate pair and linking accepted
ones through slots 0x58 / 0x5c. The observed crash was in the triangle leaf
(`PolygonAdjacencyTest+0x87d`).

### Why the shipped assets are not evidence that this is safe

**14 shipped files contain degenerate-after-weld triangles** — `corps
building.RIF` has 22, `gastowerfrag.RIF` 14, `boulders2.RIF` 9 — and the game
plays fine. That is not because the geometry is harmless:

> Every one of the 14 is under `RIF\Objects` or `RIF\Units`. **No file under
> `RIF\Levels` contains a single one.**

Only level map geometry reaches `LoadOrBuildSectionAdjacency`, via `ToMap`. A prop
or frag model never goes through section adjacency, so its degenerate triangles are
merely invisible (zero area) rather than fatal. Measured across all 563 shipped
`.rif` files.

### Reproduction

Register a script-defined level (`src/CustomLevel`) whose `map.rif` names a mesh
containing two degenerate-after-weld triangles that share the duplicated vertex,
and start it. Confirmed twice on a custom level built from a Blender export:

- **Predicted from the file alone** — 20 degenerate triangles among 244,883, giving
  48 overflowing pairs — then reproduced at that exact instruction.
- Removing **only** those 20 triangles let the level load — 18 s, 141 roles, and a
  `Test_Level_Parsed.map` written, which no crashing run ever reached. Nothing else
  changed.

An earlier revision of the same mesh had 245,598 polys with 65.7% duplicate vertex
positions and 211 degenerate-after-weld triangles, and crashed identically.

### Detecting it before it crashes

`blender/io_scene_rif/shapes.py`:

```python
welded = shp.weld_map(verts)              # quantized positions -> first index
shp.welds_degenerate(tri, welded)         # does this triangle lose a corner?
```

The exporter drops such triangles in `_shape_chunk_from_mesh` and reports the count
as a **warning** (`stats["degenerate_faces"]`) — they have zero area and render
nothing, so dropping them everywhere is cheaper than working out which shape will
become a map. `tests/test_scene.py` accounts for that loss alongside the duplicate
faces Blender cannot hold; the two are counted disjointly, in pipeline order.

This covers the quad half too, without needing to know about it: a `NavQuad` is built
from two triangles that survived, so a mesh with no degenerate-after-weld triangle
cannot produce a degenerate quad by merging.

### Why the crash is hard to see

Two traps, both of which cost time here:

- **Under a debugger there is no WER dump**, because the debugger takes the
  exception first. A crash "with no log" is the expected appearance, not evidence
  that nothing crashed. Run without one — see the recipe below.
- **It is not an AV**, so `sxe av` does not catch it and the WER entry reads
  `0xc0000409` at `__report_gsfailure`, hundreds of KB from the real culprit. The
  faulting frame is the *caller* of `__security_check_cookie`.

### If we decide to fix it in the game

Hooking slot 0x50 is unattractive — it is a per-polygon-pair predicate called from
inside the grid sweep (every polygon against the contents of 26 neighbouring
cells; the call count was not measured, but it scales with polygon count and is
plainly hot), and it is a vtable slot on a polygon rather than a free function.
Fixing the asset costs nothing and is where the exporter guard already sits.
Recorded here only so nobody re-diagnoses it.

### Ghidra

`PolygonAdjacencyTest` @ 0x0048ecf0, `NavQuad_AdjacencyTest` @ 0x0048f580 and
`BuildPolygonAdjacencyGrid` @ 0x0048aa00 were all `FUN_`-named; each now carries a
plate comment with the above. `RET`-form and arity were not re-derived — the calling
convention comes from the call site at 0x0048ab35 (`MOV ECX,EBX` / `PUSH EDI` /
`CALL [EAX+0x50]`).

Every offset in the table above is read off the decompilation of the function it
describes. **Do not derive the quad's numbers from the triangle's by analogy** —
they agree, but the two frames are laid out independently and nothing guarantees
that.

---

## 6. `TexMergePolys` @ 0x005d7590 writes a UV list before deciding not to merge

Reachable only from an **authored** `SHPMRGDT`, which is why it is filed here rather
than as a shipped-data bug: no shipped table names a non-adjacent pair.

`TexMergePolys` fuses two triangles' UV lists into `shape->uv_list[UVListIndex(a)]`
at 0x005d777a — and only *then*, at 0x005d779e, tests `out->vert_ind[3] !=
out->vert_ind[0]`, which is its implicit "do these two actually share an edge?"
check. A pair that matches on `engine_type`, texture index, flags and shared-vertex
UVs but does **not** share an edge therefore fails the merge, the caller emits both
triangles unmerged, and `a`'s UV record has already been overwritten to
`num_verts = 4` holding four fused coordinates. `a` then renders as a triangle
against a UV list claiming four vertices.

Consequence for a generator: a `SHPMRGDT` pairing must name **edge-sharing**
triangles, not merely compatible ones. The three earlier bail-outs inside the walk
are clean; this is the only one that has already mutated shared state.

---

## 7. `Role::interface_beam_script` is shared by pointer to every actor, and freed per actor

**Latent in the shipped game — and GkPlus is what makes it reachable.** Two failures, in this
order: a use-after-free when a second interface beam completes, and a **pool double-free** when a
second actor of the role is destroyed. The second needs no beam completion at all.

`AddInterfaceBeamVulnerability` @ **0x00510fe0** (`__fastcall void(Actor * /*ECX*/)`) runs from
`SpawnRole` @ 0x00503785 and `ServerSpawnActorForTeam` @ 0x005036d0 — i.e. **on every actor
spawn**, gated only on `role->interface_beam_delay` (`Role+0x80`) `>= 0`. It copies the role's
string **by pointer** and marks the vulnerability actor-scoped:

```
0051106e  MOV ECX,dword ptr [EAX + 0x88]   ; EAX = actor->role, +0x88 = interface_beam_script
00511074  MOV dword ptr [EDI + 0x10],ECX   ; EDI = the fresh 0x1c Vulnerability
00511080  MOV byte ptr [EDI + 0x18],1      ; actor_scoped = 1
```

So every actor of that role holds the *same* `char *`, and two things then free it:

- **`Actor::Destructor` @ 0x0052da98** tests `actor_scoped` and, when set, pool-frees
  `vuln->script` before freeing the 0x1c record. Two live actors is therefore enough.
- **The `VulnerabilityType::SCRIPT` (4) arm of `CharacterActor::Update`** — jump table
  @ 0x005409c4, arm at 0x0053f7de, falling into 0x0053f892 — queues the script and then frees it,
  **nulling only the `Vulnerability`'s copy** at `+0x10`:

  ```
  0053f8a8  CALL 0x00505080          ; QueueScriptExecution(vuln->script)
  0053f8b3  CALL 0x005e3f7b          ; free -- a bare JMP to pool_free @ 0x005715b0
  0053f8be  MOV dword ptr [EAX+0x10],0   ; only Vulnerability::script is cleared
  ```

**Nothing ever resets `Role+0x88`.** Its only two writers are `CreateRole` @ 0x004adee7 (zero) and
`ToRole` @ 0x0047d4c7 (a fresh allocation), and `RoleDtor` @ 0x004ada50 never touches it — so the
field also *leaks* at level teardown whenever nothing else freed it, and the dangling window lasts
the whole level.

**Why it never fires in the retail game.** `ToRole` allocates `+0x88` only when
`interface_beam_effect` (`Role+0x84`) is `4` (SCRIPT). Across all **226** shipped `.gls`/`.gsh`,
`interface beam effect` appears 6 times and is **always `destroy`** (`level07.gls` ×5,
`technocrate.gsh`), and `interface beam script` appears **zero** times. The field is NULL in every
shipped role, so both frees are guarded by a `TEST`/`JZ` and nothing happens. The 94 `.gcs`
`vulnerability … interface_beam` lines go through `CommandVulnerability`, which is **correct**: it
`strdup`s (malloc thunk @ 0x0044a870, stored at 0x0044a892) and sets `actor_scoped = 0` on its
role fan-out path (0x0044a8cb) so only `RoleDtor` frees it. `AddInterfaceBeamVulnerability` is the
**only** site in the binary that pairs `actor_scoped = 1` with a string it does not own.

**What makes it reachable is entirely GkPlus**: `make.role({interface_beam_script})`, a
`gls`-authored role, or a mod `.gsh` that sets `interface beam effect script`. Any of those puts a
non-NULL pointer in `Role+0x88` and arms both frees. GkPlus's `ScriptQueueSystem` hook on `ToRole`
is *not* the cause — it replaces the string's contents with an envelope using the same allocator
(`GameStrdup` / `pool_free`), leaving ownership exactly as `ToRole` left it — but that hook is why
the field is worth caring about here.

The fix, if one is wanted, is the engine's own correct pattern: give each actor its own copy, the
way `CommandVulnerability` does. Setting `actor_scoped = 0` instead would stop the destructor
double-free but not the completion free, which is unconditional on a non-NULL pointer.

---

## 8. `Font_QueueText` @ 0x005782e0 dereferences null when its node allocation fails

**Status:** open, unpatched. **Not reproducible in practice** — see below.

**Severity:** fatal if it ever fires, but gated behind an allocation failure that nothing
in normal play can reach. Recorded because it is cheaper to have written down than to
rediscover, not because it needs fixing.

The second defect in this function; §1 is the one that actually bites.

### Mechanism

Having laid the text out and copied it, the function allocates the 0x34-byte queue node
and does **not** check the result:

```
00578899  XOR  ECX,ECX                     ; ECX = 0 on the failure path
...
005788a7  MOV  dword ptr [ECX + 0x8],EAX   ; <-- write through null
```

`pool_alloc` (via the `malloc` thunk @ 0x005e3f72) returns null on failure, the failure
path zeroes ECX, and the very next thing the function does is store the list linkage
through it. A plain null write at `+8`, so it faults in the first page and looks like an
ordinary access violation with no useful context.

The text copy leaks on the same path: `malloc(strlen+1)` @ 0x005787f4 has already
succeeded and nothing frees it before the store. That is the lesser of the two problems
and it never gets the chance to matter, because the store faults first.

### Why it is not reachable

`pool_alloc` @ 0x00571470 is a page sub-allocator that falls back to the real CRT
`malloc` for large blocks (see `src/Memory.h`). A 0x34-byte request failing means the
process is already out of memory in a way that a 32-bit game from 2000 does not
ordinarily reach — and if it did, this would be one of many things to fall over. Nothing
about the text path makes it more likely here than anywhere else.

That is the whole reason this is a footnote rather than a fix: the failure mode is real
and the code is genuinely wrong, but no input, no asset and no script can steer it.

### Consequences for GkPlus

None that need action. `gk::QueueText` (`src/Font.cpp`) forwards to this function, so a
script calling `text.draw` inherits the same non-reachable path — it neither introduces
nor worsens it. Worth knowing only if a crash ever lands at 0x005788a7, where the answer
is "the machine was out of memory", not "the text was malformed".

The mirror deliberately does **not** guard against this. There is nothing to guard: the
allocation is the engine's, inside the call, and a wrapper cannot observe it.

### Ghidra

`Font_QueueText`'s plate comment records §1's stack smash and the parameter analysis.
This defect is deliberately kept out of it: the plate should describe what a caller can
actually hit, and this is recorded here instead.

---

## 9. `ToRole` @ 0x0047cc20 dereferences a null `Hierarchy` when a role's `.rif` is missing

**Latent in the shipped game — and GkPlus's `levels.start` is what makes it reachable.** A role
whose `shape` names a `hierarchy` section whose `.rif` file is absent gets a **NULL**
`Role::hierarchy`, stored unchecked, and `ToRole` then calls a `__thiscall` on it. Instant
`0xc0000005` at **0x005948bd** during level load, before a single frame is drawn.

### Mechanism

`ToRole` converts the role's `shape` field (parsed field id 5, `ParsedRole+0x260`) and stores the
result without testing it:

```
0047ccb4  MOV ECX,dword ptr [EBX + 0x260]   ; role field id 5 = `shape`
0047ccbc  JZ  0x0047ce1c                    ; absent -> skip the whole block
0047cced  CALL 0x0047c390                   ; ToHierarchy
0047ccf2  MOV dword ptr [EDI + 0x1c],EAX    ; Role::hierarchy <- NULL, unchecked
```

`ToHierarchy` @ 0x0047c390 tail-jumps to `GetHierarchy` @ 0x004ae390 (`__fastcall
void *(char *file /*ECX*/, char *name /*EDX*/)`), which on a cache miss calls `LoadOrGetRifFile`
@ 0x004ae960 then `BuildHierarchy` @ 0x005a9cc0 and **falls to `XOR EAX,EAX` @ 0x004ae45f if
either returns zero** — silently, with nothing printed. A missing `.rif` therefore arrives as a
bare NULL.

The call that faults is guarded, but **on the wrong thing** — the hotspot *name string*, never the
hierarchy pointer:

```
0047cd7a  MOV EAX,dword ptr [EBX + 0x260]   ; the parsed hierarchy section
0047cd80  MOV EAX,dword ptr [EAX + 0x250]   ; its field id 3 = `hotspot`
0047cd89  TEST EAX,EAX
0047cd8b  JZ  0x0047cda3                    ; THE ONLY GUARD
0047cd8f  CALL 0x0044e1a0                   ; strdup
0047cd97  MOV ECX,dword ptr [EDI + 0x1c]    ; Role::hierarchy == 0
0047cd9a  PUSH ESI                          ; ESI = EDI+0x2c = &Role::hotspot_point
0047cd9b  MOV dword ptr [EDI + 0x44],EAX    ; Role::hotspot = strdup(name)
0047cd9e  CALL 0x00594890                   ; HierarchyResolveNamedPointPos
```

`HierarchyResolveNamedPointPos` @ **0x00594890** (`__thiscall bool(Hierarchy *this, Vec3f *out_pos,
char *node_name)`, `RET 0x8` @ 0x00594a00) recursively searches the node tree for `node_name`. It
dereferences `this` at the third instruction of its body with no null check — `MOV ECX,[EDI+0x88]`
at **0x005948bd**, `EDI` having just been loaded from `ECX`. `+0x88` is the node's name, compared
with `__stricmp`. There is a second, identically unguarded call site at 0x0047cdd4 for
`alternate hotspot`.

So the condition is **missing `.rif` AND a `hotspot` on the hierarchy** — a missing file alone is
survivable. `level02` is the natural control: `warflash.gsh` names `units\scarflash_shadow.RIF`,
which is not in the install, but declares no `hotspot`, so the `JZ` at 0x0047cd8b skips the call
and the level loads with a null hierarchy in that role. This is why "the file is missing" is not
by itself a prediction of a crash.

`ConvertParsedObjects` @ 0x004747b0 converts **every** parsed section in the `#include` closure
(`MOV ECX,[EDI+0xc]; MOV EAX,[ECX]; CALL [EAX+0x1c]` — vtable slot 7, the `ToXxx` dispatch), so the
role does not have to be placed on the map. Including the header is enough.

### Which maps, and why retail never hits it

Four shipped `.gls` `#include` unit headers whose `.RIF` files were never shipped — only the
`.gsh` are present:

| map | roles | missing `.rif` |
|---|---|---|
| `mplay_bombsite`, `mplay_canyon`, `mplay_dockyard` | 10 each | `units\` `fishy`, `frogs head`, `Penguin`, `Gunical`, `Guncraft`, `Klig ship`, `X-rotuse`, `Stingray`, `Tulip`, `Shouldercrab` |
| `mplay_tf_oilrig01` | 3 | `units\` `creeper`, `stalker`, `claw` |

All ten carry a `hotspot`, so all ten fault. Every campaign level, `prison`, `Maze`,
`Training_Level`, `cityruins` and `junkyard` are clean.

**The retail game cannot reach any of the four.** `gl.exe` hardcodes its multiplayer map list as
seven `.gls`/`.gcs` string pairs — `mplay_atlantic`, `mplay_carpark`, `mplay_machine`,
`mplay_mountain`, `mplay_rorschasch`, `mplay_warehouse`, `mplay_zorro` — and the four defective
maps are exactly the ones **not** in it. They are dev leftovers: `mplay_bombsite`, `mplay_canyon`,
`mplay_dockyard` and `mplay_tf_oilrig01` have no level `.RIF` under `RIF\Levels` either, so they
could not load even with the roles fixed. The same defect sits in the unshipped `railway.gls`,
`test_level.gls` and `training05.gls` (5/11/10 roles), which likewise have no `.map`.

What makes it reachable is that `levels.start` takes a script name directly, so it will happily
load a `.gls` the menus never offer.

**All seven maps the exe does list load fine** under `levels.start` — measured, one process, back
to back: carpark 149 actors, machine 79, mountain 69, rorschasch 107, warehouse 113, zorro 102, and
atlantic was already known good. Only `mplay_atlantic` has a `.map`/`.cut` sidecar, so the other
six spend ~10 s building caches on first load and read as `Responding=False` while they do; that is
the cache build, not a hang.

### GkPlus is not implicated

The stack is the same in both dumps, byte for byte:

```
gl+0x1948bd                              ; HierarchyResolveNamedPointPos, this == NULL
gl+0x7cda3                               ; ToRole+0x183
d3d8!gk::...::HookedToRole+0x66          ; the CALL to the original
gl+0x747e4                               ; ConvertParsedObjects
d3d8!gk::...::HookedConvertParsedObjects+0x19
...
d3d8!gk::StartLevel+0xa2
```

Both detours appear only as the frame that *called the original* — `HookedToRole` reads the
`parsed+0x1b60` cache slot and then calls `ToRole`, and its own work runs after the return it never
gets; `HookedConvertParsedObjects` adds a null check and calls through. The whole chain
0x0047ccb4 → 0x0047cd9e → 0x005948bd is stock code on stock data. The mod VFS cannot be blamed
either: it only *adds* files, and these are absent from the install entirely.

### Reproducing

```
levels.start({script: "mplay_dockyard.gls", console: "mplay_dockyard.gcs"})
```

kills the process immediately — exit `0xc0000005`, WER `Fault offset 0x001948bd`, faulting module
`gl.exe`. It is **not** the hang it looks like from the harness: the REPL socket dies with the
process, and per the note below a crash always presents as a socket timeout. Dumps:
`gl.exe.46176.dmp` (2026-08-06 14:15, the original harvest batch) and `gl.exe.23876.dmp`
(deliberate repro).

### Ghidra

`HierarchyResolveNamedPointPos` @ 0x00594890 has the `RET 0x8` evidence and the null-`this` hazard
in its plate comment; `ToHierarchy` and `GetHierarchy` have plate comments recording their silent
NULL returns; both call sites (0x0047cd9e, 0x0047cdd4) carry a pre-comment naming the missing null
check.

---

## 10. Ambient sound volume (`V` in a `DUMOBJTX`) is parsed and then discarded

Every positional ambient sound in a Gunlok level is a `DUMOBJTX` text chunk on a `DUMMYOBJ`
(`rif_chunk_format.md`, "Ambient sound is `DUMOBJTX`"), whose third line carries directives:

```
Sound
GL_Wind03.wav
V40 P0 R0
```

`ToMap`'s inline parser reads `V` correctly - digit accumulation, sign, a default of 100 - and
passes it as the 5th argument to `SoundSystem_AddAmbientEmitter` @ 0x0058b9e0. **That function
never reads the argument.** A full instruction sweep of 0x0058b9e0..0x0058bc37 returns zero
references to the volume's stack slot (`[EBP + 0x18]`); the emitter's volume field at `+0x10` is
filled from the *sample's* own default instead (`0058bb0d MOV EAX,[ESI + ECX*0x1 + 0x10]`,
stored at 0058bb96). `P`, `I` and `R` in the same call **are** read, so this is one argument
dropped rather than the whole parameter block being ignored.

**Consequence:** 514 of the 1,097 shipped emitters carry a `V`, using 11 distinct values from 20
to 100, and every one of them plays at its sample's default volume instead. So the level
designers' ambient mix is not what you hear, and no amount of editing that number changes
anything.

Not worth "fixing" in GkPlus - it is authored intent that the shipped mix was balanced around,
so honouring it now would change how every level sounds. It matters because a tool that exposes
these for editing must say the field is inert, exactly as `CUTEVENT` kind 5 is.

**A second, latent one in the same parser:** the directive letters are dispatched through a jump
table keyed `'I'`..`'V'` via `ADD -0x49` / `CMP 0xd` / `JA`, so **lowercase `v`/`p`/`r` are
skipped in silence** - while line 1's `"Sound"` test is `lstrcmpiA` and *is* case-insensitive.
Nothing shipped trips it (all 1,540 directives are uppercase; 14 of the 1,097 first lines are
lowercase `sound` and work fine), but anything authoring a `DUMOBJTX` must emit uppercase
directives or they vanish with no error.

---

## 11. The pool allocator has a critical section, gated on a flag nothing ever sets

`pool_alloc` @ 0x00571470 and `pool_free` @ 0x005715b0 are the game's page sub-allocator, and
**everything** goes through them: `malloc`, `free` and `strdup` are JMP thunks into this, so
every actor, role, list node, hash node and string in the process comes from here.

It has a lock and does not use it. At 0x00571473 the prologue is
`CMP byte ptr [0x007c066c], 0x0` / `JZ`, which skips the `EnterCriticalSection(0x007c0670)` at
0x00571487. That byte has **exactly four references in the whole binary and all four are
reads** - 0x00571473, 0x0057158e, 0x005715bf, 0x00571659, i.e. the same test at the top of each
of the four entry points. Nothing writes it, so it is zero for the life of the process and the
critical section is never entered. The per-size free lists @ 0x007ba668 and the page headers are
therefore **unsynchronised**.

This matters because the allocator is genuinely reached from both threads. The executor calls
`SpawnRole` @ 0x00503710 from `Frag`, `DropItem`, `AiThink_Node`, `OnPrePhysics`,
`MobileActor::Update` and `MultiplayerRespawnRole`, and `Actor::Ctor` @ 0x0052d1f0 ends in an
inlined hash insert that `pool_alloc`s its node - while the main thread allocates from the same
lists for its own work.

**It is presumably why the flag exists**: someone built the lock, gated it on a "are we
threaded?" byte, and never set the byte. Whether it is *actually* harmful in the shipped game is
not established here - Gunlok has shipped like this for 25 years - and it is listed as a trap
rather than as a crash with a repro.

What it means for GkPlus is the part worth acting on: **a lock inside `gk::pool_alloc` /
`gk::pool_free` would be theatre.** Our wrappers are not the allocator, and the game's own call
sites - which are almost all of them - would walk straight past it. So `src/Memory.cpp` is
deliberately left unsynchronised, and the mitigation available to a caller is the one the engine
itself uses for world mutation: park the other thread with the `SuspendExecutor` /
`ResumeExecutor` handshake (`gk::ExecutorPause`, see `src/Misc.h`) around anything that allocates
or frees pool memory while the simulation is live. That is what the GkPlus paths in
`vulkan_renderer_notes.md` §4.73 now do.

The only fix that would cover every caller is to `InitializeCriticalSection(0x007c0670)` at load
and then set 0x007c066c to 1 - turning on the lock the developers wrote. Not done: it changes the
behaviour of every allocation in the process to fix a hazard nothing has yet been shown to hit,
and the critical section's own initialisation state is unknown.

---

## 12. The HUD's meters and item icons are drawn under the world's depth slice

**Status:** fixed in GkPlus by `HudFixSystem` (`src/HudFix.cpp`). `GKPLUS_HUD_FIX=raw` restores
the game's own behaviour.

**Severity:** cosmetic, but total — in the shipped game **no character's health meter, armour
meter or item icons are ever visible**, in any level, for the whole game. The panel plate behind
them is drawn opaquely on top, so the meter troughs and the item slots read as empty decoration.

**Reproduces without GkPlus.** `d3d8.dll` renamed aside, version stamp back to the stock
"v1.3 DX8", Single Player → Training Level → Area 1: the Gunlok panel shows empty troughs and
empty item slots. Nothing of ours is in the process.

### The two halves of the HUD take different paths to D3D

`rendering_notes.md` §4.4 has the layering mechanism; the part that matters here is that the two
halves of one panel are drawn by different machinery:

- the **panel plate** is a retained render-queue item. All 11 `RenderQueue_Submit` sites in
  `HudItem_DrawByKind` @ 0x0055fbd0 pass `Camera_Hud` @ 0x007b4e40, so it is drawn under that
  camera's viewport slice, `MinZ 0.03 .. MaxZ 0.04`.
- the **meters and icons** never reach the queue. `Hud2D_DrawQuad` @ 0x005695c0 appends them as
  pre-transformed quads to a shared immediate-mode batch, with an authored **`z = 0.03f`** —
  exactly `Camera_Hud`'s `MinZ`, the front of the HUD slice. A batch carries no camera; it is
  drawn under whatever viewport is current at its single flush.

The authored `0.03f` is the whole of the intent: the meters were meant to be at the front of the
HUD slice, in front of the plates.

### The bug is one instruction's worth of frame order

In `RunInGameFrame` @ 0x0046e6c0:

```
0046e8b8  CALL Hud2D_BeginBatch     ; 0x005695a0, opens the batch
0046e8c1  CALL RenderHudItems       ; 0x0055fb20, makes Camera_Hud current, emits the meter quads
0046e8ca  CALL DrawOrderMenu        ; 0x00498610, makes Camera_World current again   <-- the culprit
0046e8cf  CALL Hud2D_FlushBatch     ; 0x00569ed0, one DrawIndexedPrimitive, viewport 0.10..1.00
...       RenderQueue_Flush         ; 0x00574c7b, and only now the plates, under Camera_Hud
```

`vulkan_renderer_notes.md` §4.45 measured, with a probe against the real D3D8, that D3D does not
run the viewport transform over a pre-transformed vertex — it **clamps**:
`depth = clamp(z, MinZ, MaxZ)`, no scale and no bias. So under the world viewport the authored
`0.03` is clamped **up to 0.1**, behind the entire HUD slice. Both the depth test and the paint
order then go to the plate, and it is opaque (`D3DRS_ALPHABLENDENABLE` off) with z-write on.

`HudItem_DrawByKind` itself is innocent: in its own source order it submits the plate *before* it
emits the meter quads, which is exactly the "meters on top of plate" intent.

**§4.45 has a paragraph about this bar and read it the wrong way round.** Its first cut used
viewport 0..1 for pre-transformed draws, which made the bar render bright green, and it recorded
that as a regression against a bit-exact region — "that bar authors a z *below* its slice, so D3D
clamps it up to `MinZ` and the panel that should cover it wins". The clamp reasoning is right and
the conclusion is not: the panel was never supposed to cover it. It is the standing hazard that
`vulkan_renderer_notes.md` §4.28 and §4.86 name — the reference can be the wrong one — showing up
in the one place nobody re-examined, because matching d3d8 exactly *is* the renderer's goal.

### What was measured

Level02 under `GKPLUS_RENDERER=d3d8`, camera settled and paused, 178 actors. Three consecutive
logged draws, all sampling `units\plates 2 1024.rim`:

```
idx  prims  fvf                                  blend  z zw  viewport MinZ..MaxZ
 65     12  0x1c4 XYZRHW|DIFFUSE|SPECULAR|TEX1    off   on on  0.1000 .. 1.0000
 66      2  0x112 XYZ|NORMAL|TEX1                 off   on on  0.0299 .. 0.0399
 67      2  0x112 XYZ|NORMAL|TEX1                 off   on on  0.0299 .. 0.0399
```

(The printed 0.0299/0.0399 are `%.4f`-truncated `0.03f`/`0.04f`.) `render.ref_hide = [66, 67]`
makes both character panels vanish and reveals filled green meter bars and the item icons
underneath; `ref_hide = [65, 67]` removes those too. That pair is what identifies each draw — draw
65 is the meters and icons, 66/67 are the plates.

### The fix, and why it splits the batch rather than re-pointing the flush

`HudFixSystem` detours `RenderHudItems` @ 0x0055fb20, calls the original, then flushes the batch
and opens a fresh one while `Camera_Hud` is still current. Draw 65 then goes out under
0.0299..0.0399 and clamps to 0.03, in front of the plates. Verified under both `d3d8` and
`vulkan`; outside the HUD panel the frame is unchanged (two large regions bit-identical across
launches, the rest at the cross-launch drift floor of `vulkan_renderer_notes.md` §4.30).

Forcing `Camera_Hud` over the *whole* flush would have been one line shorter and would have moved
a second thing: **`DrawOrderMenu` appends to the same batch**, one more meter bar via
`Hud2D_DrawMeterBar` @ 0x0049b322 with `z = 0.1f`, authored for `Camera_World` — it is a unit's
health bar floating over the world, not a panel element. One flush is one `DrawIndexedPrimitive`
under one viewport, so the batch has to be cut in two for the two depth intents to survive.
Cutting it is a transition the engine already performs itself: `Hud2D_DrawQuad` does exactly
`RenderBatch_End` / `Draw` / `Begin` at 0x005695e6 when the vertex buffer fills.

Two facts make the seam safe, both measured rather than assumed. `RenderHudItems` has **exactly
one call site** (0x0046e8c1) and **zero literal occurrences** of its address anywhere in
`.text`/`.rdata`/`.data` under an unaligned byte-by-byte scan — so it is in no vtable and no
callback table, and it is always reached inside an open `Hud2D_BeginBatch` window (0x0046e87a on
the inventory branch, 0x0046e8b8 on the in-level one). `Hud2D_FlushBatch` is the same: one call
site, no literal references. And an empty batch is free — `RenderBatch_Draw` opens with
`CMP dword ptr [ESI+0x50], 0` and returns — so the frames where `HudItemList` is empty, on which
the original returns before making `Camera_Hud` current, cost a lock/unlock pair and nothing else.

One thing this does **not** settle: `Camera_Hud`'s ZFUNC is `D3DCMP_LESSEQUAL`, so a plate whose
NDC z were exactly 0 would tie at 0.03 and, being drawn later, would still win. It evidently is
not — the meters render — but the margin is unmeasured. If a future change makes them flicker,
the tie-proof variant is to write a degenerate range (`cam+0x264 = cam+0x268 = 0.029f`) into
`Camera_Hud`'s viewport for the batch draw only; `Camera_UpdateDepthState` rewrites both from
`+0x19c`/`+0x1a0` on the next change, so a missed restore self-heals.

---

## 13. Three unchecked writes reachable from any peer in a multiplayer session

**Status:** confirmed by disassembly, unpatched, and reachable by anyone in the session. These are
defects in **Gunlok**, not vulnerabilities in GkPlus - GkPlus neither introduces nor widens them,
and it is a game from 2000 with no vendor to notify. `directplay_protocol_notes.md` section 6.1
records them beside the wire format; this is the fuller write-up.

The executor services a handful of command ids in an **if-chain before** its jump table
(0x005091b8-0x0050922f), so the table's range check on the id never covers them and each arm is
responsible for checking its own payload. Two do not.

- **Command `0x2d` writes one byte at a wire-controlled 32-bit offset.** The arm at 0x005091f0 is
  `MOV byte ptr [EAX + 0x7b7147],1` with `EAX` loaded straight from `payload[0]`. The evidence that
  this is an omission rather than a deliberately wide range is its **sibling writing the same
  cell**: `0x2e` at 0x005091fc does `DEC EAX ; CMP EAX,4 ; JNC ___report_rangecheckfailure` first.
  One arm range-checks 1..4; the neighbouring arm does not check at all.
- **Command `0x2a` writes four bytes at a wire-controlled offset, with no check on any of its three
  arguments.** `*(int *)(0x007b70e4 + (arg0 * 5 + arg1) * 4) = arg2` at 0x0050921c-0x00509228 -
  both indices and the stored value come from the payload. The intended target is the
  `MaxUnitsPerTeam` region; nothing constrains it to that.
- **Three unbounded inline `strcpy`s on the `0x28` / `0x8d` chat-and-player-name path.** The wire
  form carries a 255-byte string inside a 259-byte message and every copy terminates only on a NUL.
  The sender `SendChatOrPlayerName` @ 0x004fcd00 copies into a `SUB ESP,0x108` frame at
  `[EBP-0x104]`, so **a long enough local chat line smashes the sender's own frame** past ~256
  bytes - the same family as section 1, and reachable with no peer involved at all. The executor's
  lobby-name arm copies to the fixed buffer 0x007b7150 at 0x005091bd-0x005091d2, and the `0x8d`
  re-broadcast at 0x00509734 copies it again into a stack frame; a peer that sends 259 bytes with
  no NUL in the text field overruns both of those.

Consequence for GkPlus: none of this is ours to fix, but anything that *exposes* the wire to script
would make all three script-reachable, which is the reason to keep them written down.

### 13.1 What a later pass added to the two index writes

Re-measured from the disassembly; the two entries above are correct and this only sharpens them.

- **The only gate ahead of either arm is a non-zero sender.** `CMP dword [EBP-0x2f8],0 / JZ <default>`
  @ 0x005091a2. The `[0x007b9df2]` gate @ 0x00509282 sits **after** all of this if-chain and covers
  none of it.
- **`0x2a`'s effective address covers the whole address space.** The arm is
  `MOV EAX,[EBX+4]` / `LEA ECX,[EAX + EAX*4]` / `MOV EAX,[EBX+0xc]` / `ADD ECX,[EBX+8]` /
  `MOV [ECX*4 + 0x7b70e4],EAX` — the index is a full 32-bit `a*5 + b` scaled by 4, so it is an
  **arbitrary 32-bit write with a fully controlled value**, not a write confined to one table.
  For the *intended* range `a` in 1..4 and `b` in 0..5 it lands in **0x007b70f8..0x007b7144**, which
  makes `a` **1-based**, matching its sibling commands. Its neighbours are `TeamUnitCounts`
  @ 0x007b70e0 (the 5 dwords `TotalUnitsAcrossTeams` @ 0x004fb8f0 sums), `LobbyRosterIndex`
  @ 0x007b70f4 and `PlayerSlotFlags` @ 0x007b7148. Whether that span is the per-player unit-selection
  table or the `MaxUnitsPerTeam` region named above is **not settled** — `FUN_004f10b0` (23 KB) was
  never decompiled — and the defect does not depend on which it is.
- **`0x2d`'s array is now typed.** `PlayerSlotFlags` is `byte[4]` at **0x007b7148..0x007b714b**,
  bounded by `NumPlayerSlots` @ 0x007b714c (= 4). The `0x007b7145-0x007b7148` reading that once
  competed with it is **refuted**: nothing in the binary touches 0x007b7145 or 0x007b7146 at any
  width. The `+0x7b7147` displacement in the arm is a **1-based index rebase**, not an array start —
  and the same idiom appears on the unrelated per-slot string array one over (written
  `MOV byte [EDX+0x7b714f],AL` with `EDX` pre-incremented @ 0x004fc1d0, read 0-based @ 0x00503450).
  **So a displacement one below an array base is this compiler's output for a pre-incremented pointer
  loop, and is not evidence about where an array begins.**

### 13.2 Command `0x34` leaks its message buffer

**Status:** confirmed, remotely triggerable, unbounded over a session.

Every other path through `ExecutorThreadProc` frees the dequeued message with `PUSH EBX; CALL free`
(0x005e3f7b). The `0x34` arm does not. `CMP dword [EBX],0x34 / JZ 0x0050adea` @ 0x00509279 jumps into
the block 0x0050adea..0x0050af16, which broadcasts (`CALL BroadcastToPlayers` @ 0x0050aeef) and then
**falls through into the periodic tick at 0x0050af17** — and 0x0050af1d immediately reloads `EBX`
from `[EBP-0x2f0]`, so the pointer is gone. A scan of 0x0050adea..0x0050af16 finds **no call to
`free` at all**. `0x34` is the only id that falls into the tick this way.

### 13.3 The `0x1d`/`0x1f` `append` flag is always 0

See §18 — kept with the order system rather than here, because it is an arithmetic slip rather than
a memory-safety one.

---

## 14. `InventoryInfo_Ctor` @ 0x00483390 leaves `pickup_radius` uninitialised

**Status:** confirmed, latent. The field reads back whatever the pool block last held.

`InventoryInfo` is 0x18 bytes and its constructor zeroes **five** dwords, not six: +0x00, +0x04,
+0x08, +0x0c and +0x14. It never touches **+0x10, `pickup_radius`**. All four call sites are in
`ToRole` (0x0047cee1, 0x0047cf8f, 0x0047cfd5, 0x0047d01a), each
`PUSH 0x18; CALL malloc; ...; MOV ECX,EAX; CALL 0x00483390; MOV [role+0x64],EAX` - so the block is
pool memory and nothing has zeroed it for them. The two later sites go on to write +0x14 and derive
a value from `ParsedRole+0x528`; **none of the four writes +0x10**. The field is therefore
uninitialised on *both* construction paths, not just one.

---

## 15. `SoundSample_ReadWholeFile` @ 0x005d3940 leaks the file handle on its failure path

**Status:** confirmed, bounded - one handle per failed read.

`__thiscall void(SoundSample *, const char *filename)`, `RET 0x4`, single caller 0x00589be8 inside
`SoundSystem_LoadSampleIntoSlot`. It does `CreateFileA(GENERIC_READ, share read, OPEN_EXISTING)`,
`GetFileSize`, `malloc`, then one `ReadFile` of the whole file into `this+0x20` with the size into
`this+0x1c`. **`CloseHandle` sits inside the `if (ReadFile succeeded)` branch**, so a failed read
returns with the handle still open. It takes an unreadable or truncated sound file to trigger,
which is why normal play never shows it - but a mod filesystem is exactly the thing that can serve
one, so it is worth knowing before blaming `src/Vfs` for a handle count that climbs.

(Adjacent and harmless: the same function frees the previous buffer with
`free_sized(this+0x20, 1)` when it was allocated at its real size. That works only because the pool
ignores the size argument.)

---

## 16. `AiBeginInvestigate` @ 0x0045e050 sets its deadline with no clock read (CONFIRMED)

**Status:** **CONFIRMED.** The one global this used to rest on has been measured, and the *stated
consequence has changed* — see "What it actually costs" below. It is neither "expires instantly" nor
"never expires".

Every sibling that arms a deadline reads the clock first - `Decoy_Dismiss` @ 0x00450f60 uses
`clock + 60 * ClockTicksPerSecond`, `Mine_OnDeployed` uses `now + 10 * ClockTicksPerSecond`,
`PostAiStimulus` calls `ReadScaledClock64(&GameTimeClock)`. `AiBeginInvestigate` does not: there is
**no clock read anywhere in the function** - no `Clock::ReadScaled32`, no `ReadScaledClock64`. Its
tail is

```
0045e18c  CALL GetCurrentThreadId / CMP EAX,[ExecutingThread]
          MOV ECX,[0x007c07e0] / CMOVZ ECX,[0x007c07b0]     ; per-thread ticks-per-second
          MULSS by [EBP+8] / FLD / FISTP -> EAX             ; seconds * tick rate
0045e1c6  MOV EDI,[0x007c07dc] / CMOVZ EDI,[0x007c07ac]
          ADD EDI,EAX / MOV [EBX+0x90],EDI                  ; deadline = that global + the delta
```

so `+0x90` becomes `[0x007c07dc] + seconds * tick_rate` rather than `now + seconds * tick_rate`.

**0x007c07dc is settled: it is ticks-per-second, a RATE.** It is `MainClock+0x0c` (`MainClock`
@ 0x007c07d0; the executor's copy is `ClockTicksPerSecondExecutor` @ 0x007c07ac =
`ExecutorClock+0x0c`), and the DB already names it `ClockTicksPerSecond`. Its **only** writer is
`MulDiv(delta_ticks, 1000, elapsed_ms)` inside `Clock::Calibrate` @ 0x00571931, and `Calibrate` is
called from **`WinMain` @ 0x0046b934 and `LoadLevel` @ 0x004e1420 only** — never per frame. A
reference sweep gives **133 reads and zero writes** (the one write goes through `ECX`). The *running*
clock is `Clock+0x18`, read through `AccumThreadClock64` @ 0x0044df20. So the rate is sitting where
the `now` term belongs, and the defect is real.

Every sibling gets it right by adding the think proc's own `now_lo`/`now_hi`: `AiThink_Bot` case 6
@ 0x00453e55 (`rate*17 + now`), `Mine_OnDeployed` @ 0x0045a75f (`rate*10 + now`), `AiThink_Minebot`
@ 0x00456fb3 (`rate*1 + now`), `Decoy_Dismiss` @ 0x00450fd8 (`now + rate*60`).

### What it actually costs — **not** "expires instantly"

Both call sites are inside `AiThink_Minebot` (0x00456f7c on itself, 0x004571b0 propagating onto
another actor), and its **`ai_state == 7` handler @ 0x00456f86** is what consumes the deadline. It
does a signed 64-bit compare against `now`, and its **expired branch @ 0x00456fa6 does not
transition out — it re-arms**:

```
00456f92  CMP dword [ESI+0x94],EAX     ; deadline_hi vs now_hi   (signed 64-bit compare)
00456fa4  JNC 0x00456fde               ; not expired -> use it
00456fa6  CALL GetCurrentThreadId ...
00456fb3  MOV ECX,[ClockTicksPerSecond] / CMOVZ ECX,[...Executor]
00456fc0  ADD ECX,dword [EBP+now_lo]   ; <- re-arm to now + rate*1
00456fc6  MOV [ESI+0x90],ECX
00456fde  SUB ECX,EAX                  ; remaining = rate, i.e. ONE SECOND
```

So the first `ai_state == 7` tick always takes the expired branch and **the investigate window is
silently replaced by ~1 second, re-armed every time**. The `seconds` argument still reaches
`GotoObject` @ 0x0045e168, so **movement is unaffected — only the timer is.**

**An uptime framing does not survive contact.** The deadline is an absolute position roughly
`1 + seconds` seconds after the accumulator's zero, and `Clock+0x18` is zeroed **once**, by
`Clock_Ctor` @ 0x00571830 at static init, and never reset — `LoadLevel`'s field-by-field
`MainClock -> ExecutorClock` copy carries the accumulated total across rather than resetting it. It
would land in the *future* only in a session that reached gameplay within `1 + seconds` seconds of
process start, which a level load makes impossible. **The behaviour is uniformly wrong, not
uptime-dependent.**

**Scope:** the `ai minebot` roles — `Rol_Walking_Mine` (identifier `"minebot"`) and
`Rol_Mini_Minebot` (`"mini_minebot"`) — plus whatever the propagation site 0x004571b0 pushes the
same broken deadline onto.

---

## 17. The in-game pause menu's Options, Load and Save cannot be opened (PROPOSED)

Two halves plus a test. The first is a proposed defect that turned out to be **refuted**; the second
is what refuting it implies, and it is the part that matters. 17.3 records the live check that was
supposed to settle the second and **could not**.

### 17.1 The null dispatch slot is real and unreachable

The proposal was: `InGameMenu__OnItemActivated` @ 0x00563c30 switches on `this->kind`
(`HudWidget+0x60`) with **no bound check**, its byte table @ 0x005649b8 maps widget kinds
0x04/0x05/0x0d/0x0e/0x0f to index 22, and index 22's pointer-table slot at 0x005649b4 is a
**genuine null dword** - so any of those five widgets, activated, would `JMP 0`. Every part of that
is true as far as it goes, and `menu_system_notes.md`'s former claim that those five ids are
"unused" is **not** the reason it is safe: three of the five (0x05, 0x0d, 0x0e) are constructed at
six sites and keep the base vtable 0x0066971c, whose slot 4 *is* this function.

It is refuted because **the function has no caller.** Four sweeps, in increasing order of
paranoia:

- 0x00563c30 appears as a dword in exactly **two** places across every initialized block, found by
  a raw 4-byte-stride scan of all blocks rather than by `getReferencesTo` (which returns 0 here -
  the undefined-vtable trap at the end of this file): 0x0066972c and 0x00669770, i.e. **slot 4** of
  `HudWidget_vtbl` and `HudMenuWidget_vtbl`. It is a vtable slot and nothing else.
- A raw `E8`/`E9 rel32` scan over all **167,364 undefined bytes** of `.text` finds no call or jump
  to it, nor to `OpenInGameOptionsMenu`, `InGameMenuAction_LoadGame`, `OpenInGameLoadMenu` or
  `OpenInGameSaveMenu`.
- Every x86 form that can dispatch vtable slot 4 was enumerated over defined **and** undefined
  regions: `CALL/JMP [reg+0x10]` - 117 raw encodings, 112 at defined instruction starts, 5 in
  undefined regions and none of those in HUD/menu/inventory code; `MOV reg,[reg+0x10]; CALL/JMP reg`
  - 30 defined, 0 undefined; `ADD reg,0x10; CALL/JMP [reg]` - 854 `ADD reg,0x10` in `.text`, **none**
  followed by such a call; `CALL/JMP [reg+reg*4]` - none in `.text`.
- Of all of those, exactly **three** dispatch slot 4 on this widget family: the tail-jumps at
  0x0056a2c3, 0x0056a2e3 and 0x0056a303 (callers `HandleKeyPress3` on DIK 0x1c Enter / 0x39 Space,
  and `ToggleInGamePauseMenu`, whose second body block *is* 0x0056a2f0). All three land on vtable
  0x006697e8, whose slot 4 is **`InGameDialogButton_OnActivated`** @ 0x0056c380 - not this
  function.

**The caveat, honestly:** this is a proven negative over dispatch *forms*, not over data flow. It
was **not** proved that no object of those two vtables can reach one of the other 109
`CALL [reg+0x10]` sites. The supporting argument is arity - `InGameMenu__OnItemActivated` ends in a
**bare `RET`**, so zero stack arguments, and 56 of the 109 push an argument first, making those
structurally a different slot-4 signature; the remaining 53 are all nav-mesh/pathfinding,
executor-thread, chunk-I/O, cutscene and image-format code, none of which holds a HUD widget.

### 17.2 The consequence: three in-game menu entries are dead (PROPOSED)

`OpenInGameOptionsMenu` @ 0x00567f00 has **16 callers and every one of them is inside that dead
body**. `InGameMenuAction_LoadGame` @ 0x004a0e70 has 2, both inside it. `OpenInGameLoadMenu`
@ 0x005686b0 has exactly **1** caller - `InGameMenuAction_LoadGame`. So in the retail build the
in-game pause menu's **Options, Load Game and Save Game cannot be opened at all**: the pause menu
draws all six rows, and three of them do nothing.

The front-end route is separate and unaffected - `Menus[36]` reaches `MenuSaveGame` @ 0x004e6d30 and
`MenuLoadGame` @ 0x004e6be0 through `OnMenuItemClicked`, so saving and loading **from the main menu
work**. It is only the in-game path that is dead.

**This half is PROPOSED and needs one cheap live confirmation before it is stated as fact.** A live
check has been **attempted and came back INCONCLUSIVE** - see §17.3. It stays PROPOSED. If in-game
Options *does* open when someone finally actuates it, the dispatch analysis in §17.1 is wrong
somewhere and this verdict must be re-derived from whichever site actually reached slot 4.

This is stock-Gunlok behaviour, reproducible with **no GkPlus loaded** - nothing in the framework
hooks that path. `menu_system_notes.md`'s in-game menu section carries the table counts and the
widget-kind construction sites.

### 17.3 The live check: INCONCLUSIVE, because the harness cannot actuate the wheel

**Run 2026-08-19.** Setup: Steam running in **session 1** (per `CLAUDE.md`'s session-0 rule); Debug
`d3d8.dll` deployed; `Start-Gunlok -Renderer d3d9 -Port 9222`; then
`levels.start({script: "level02.gls", console: "level02.gcs"})` over the REPL. **178 actors /
274-279 draws**, matching `utils/rendertest`'s documented level02 figures, so the level was genuinely
up and being drawn.

**Verdict: INCONCLUSIVE. §17.2 stays PROPOSED.** The in-game wheel's only actuator is a real mouse
click, and no synthetic click reaches this game - so the in-game nulls this run produced carry no
weight in either direction. That the failure is in the **actuator rather than in the game** is
demonstrated rather than assumed: the same four click methods also failed on the **front-end** menu,
whose activation is independently known to work (point 4 below).

What the run *did* establish, all measured:

1. **The wheel opens.** ESC -> `DeselectOrActivateMenu` @ 0x00496f70 -> (selection empty) ->
   `ToggleInGamePauseMenu`. All six rows render exactly as `menu_system_notes.md` documents: Resume
   Play / Options / Load Game / Save Game / Restart level / Exit to Menu. **ESC is a toggle** - a
   second press closes it again.
2. **Mouse *position* reaches the wheel.** Hovering moves the highlight between rows, so
   `InGameMenuSelectedItem` is being updated and the wheel's per-frame hit-test is running.
3. **The wheel takes no keyboard input at all.** With the wheel verifiably open, DOWN does not move
   the highlight, and neither ENTER nor SPACE activates the selected row. It is **mouse-only**.
4. **The positive control passed, on the keyboard.** At the front end, two DOWNs then ENTER
   activated "Exit Game" and the process **exited cleanly** - `<Gunlok>\scripts\GLkeys.cfg` was
   rewritten (`CLAUDE.md`'s documented clean-exit signal) and **no WER entry** was produced. So
   `OnMenuItemClicked` @ 0x004ecf10 is live and the synthetic *keyboard* path is real.
5. **No synthetic mouse click reaches either menu.** Four methods, each failing at the **front-end
   control** as well as in-game:
   - `SendInput` LEFTDOWN+LEFTUP instantaneous;
   - `SendInput` with the button **held 250 ms** (~15 frames, so not a polling-edge miss);
   - `PostMessage WM_LBUTTONDOWN`/`WM_LBUTTONUP` at **physical** client coords;
   - the same at **virtualized** coords (gl.exe is not DPI aware - window DPI 144, scale 1.5,
     physical client 3060x1716, so the two coordinate spaces differ and both were tried).

   Consistent with `input_notes.md`: the mouse is on **Raw Input**, so the window procedure ignores
   `WM_LBUTTON*` entirely, and this game evidently does not accept injected raw button events
   either.

**One bonus finding, filed under §3.** Ending this run with `screen.main_menu()` from the live level
crashed the process at fault offset `0x00179009`, which is **§3**'s documented `GetResourceString`
signature rather than a teardown fault — so it is recorded there, as §3's deterministic repro, and
not here or under §4.

**A caution for whoever runs the next one, which is the most reusable part of this run.** The first
attempt pressed DOWN/ENTER/SPACE **after** a second ESC had already closed the wheel - ESC is a
toggle - so those three "nothing happened" observations were **void**, and they looked exactly like
a confirmation of §17.2. Re-running with the wheel verifiably open produced the same nulls, but by
then the control in point 4/5 had shown the actuator itself was dead. **A null result plus an
untested actuator reads identically to a confirmed defect**, which is how this nearly got recorded
as CONFIRMED. Establish that your input method can activate *something* that is known to work,
in the same session, before reading any silence as evidence.

**What would settle it now.** Two options remain, and neither is another scripted run:

- **A human physically clicks "Options" in the pause menu.** Five seconds, and decisive.
- Or a **temporary GkPlus detour on `InGameMenu__OnItemActivated` @ 0x00563c30 that logs on entry**,
  plus one physical click. That additionally distinguishes "never called" from "called but its
  case body is inert" - which no screenshot can.

---

## 18. A queued attack-ground order can never be a close-range one

**Status:** **cause CONFIRMED, effect CONFIRMED.** (This entry was going to be filed as
"cause measured, effect unknown"; the consumer had already been read — see "What it costs" — so both
halves are settled. `orders_notes.md` §8.2 is the same finding from the order system's side.)

`ExecutorThreadProc`'s arm at 0x00509e14 computes `CharacterActor::QueueOrderPosition`'s `append`
argument from the command id:

```
00509e2b  CMP EDI,0x20        ; EDI = the wire command id
00509e33  SETZ CL             ; append = (id == 0x20)
00509e39  PUSH ECX
00509e48  CALL dword [EDX + 0x158]    ; Actor slot 86 QueueOrderPosition
```

**That arm is reached by exactly ids `0x1d` and `0x1f` — never `0x20`.** Decoded from the fully
resolved switch: byte index table @ 0x0050bae0 into targets @ 0x0050ba3c, index `id - 4`, bounded
`CMP EAX,0x39 / JA`. So `ZF` can never be set there and **`append` is always 0**: a queued *position*
order can never set its append byte.

It is a **copy-paste of the constant from the sibling arm**. The four arms and their compares:

| ids | arm | compare | correct? |
|---|---|---|---|
| `0x0a`/`0x0c` | 0x00509e91 | `CMP EDI,0xc` @ 0x00509ea8 | yes |
| `0x1d`/`0x1f` | 0x00509e14 | `CMP EDI,0x20` @ 0x00509e2b | **no — always false** |
| `0x1e`/`0x20` | 0x00509e5c | `CMP EDI,0x20` @ 0x00509e6e | yes |
| `0x0b`/`0x0d` | 0x00509fae | — | — |

The queued *target* arm (`0x1e`/`0x20`) is the one where `CMP EDI,0x20` belongs, and it is correct
there.

### What it costs

The flag lands in `PendingOrder+0x20`, and the **kind-1 consumer passes it straight through as
`AttackPosition`'s `close_range`** — `CharacterActor` slot 70 @ 0x0053d8d0:

```
0053db9e  MOVZX EAX,byte ptr [ECX + 0x20]   ; rec->flag
0053dba4  PUSH EAX                          ; -> close_range
0053dba5  PUSH 0x0
0053dba7  PUSH dword ptr [EBP + 0x10]       ; order time
0053dbaf  PUSH EAX                          ; &rec->position
0053dbb0  CALL dword ptr [EDX + 0x184]      ; Actor slot 97 AttackPosition
```

So **a queued attack-ground order can never be a close-range (lobbed) one.** The immediate path is
unaffected: it supplies `close_range` correctly as `(id == 0x0c)` at 0x00509eab. Kind 0 does the same
through slot 96 at 0x0053dbdd, and *its* arm's compare is correct, so only the position variant is
affected.

Worth noting the naming trap this defect sits on: the argument is `close_range`, not `append`. The
executor arm looks like it is computing "should this be appended to the queue" — and if it were, an
always-0 flag would be far more visible. It is not; the record is appended unconditionally.

---

## 19. Four uninitialised bytes reach every savegame containing a waypoint

**Status:** confirmed, harmless, but it makes saves non-reproducible.

The waypoint record is `pool_alloc(0x18)` and has three allocators, all writing the same shape:

| function | list | RET |
|---|---|---|
| `MobileActor::PushRouteWaypoint` @ 0x0053a640 | push-**front** onto `+0x204` | `RET 0xc` |
| `MobileActor::AddWaypoint` @ 0x0053a760 (slot 90) | push-**back** onto `+0x204` | `RET 0x10` |
| `MobileActor::AppendPatrolPoint` @ 0x0053a830 | push-**back** onto `+0x214` | `RET 0xc` |

Layout: `+0x00 Vec3f pos`, `+0x0c int keep_on_arrival`, `+0x10 float wait_time`, and **`+0x14` —
which none of the three ever writes.** Verified by reading all three bodies: each stores `[EBP+0xc]`
into `record+0x0c` and `[EBP+0x10]`/`[EBP+0x14]` into `record+0x10`, and there is no store to
`record+0x14` anywhere.

Both savegame paths move **the full 0x18 bytes**: `WriteActorFixups` @ 0x0053210d
(`PUSH 0x18; PUSH [EAX+0xc]`) and `ReadActorFixups` @ 0x00531060 (`PUSH 0x18` … `LEA ECX,[ESI+0x204]`).
So **4 bytes of uninitialised pool memory are written into every `.sav` holding a waypoint.** Nothing
reads the field, so the effect is bounded at "saves are not byte-reproducible" — which matters only
if anything ever diffs two saves of the same state.

### 19.1 And `keep_on_arrival` is a dead branch

The reader is `FUN_0053a1d0` @ 0x0053a2cf: zero pops the record and `free_sized(rec, 0x18)`; non-zero
advances the cursor and **retains** it. **Every direct writer in the shipped binary passes literal
0** — all four `PushRouteWaypoint` call sites (0x005398ea, 0x0053c633, 0x00452ae4, 0x00452b0d) and
both `AppendPatrolPoint` call sites (0x0045489a, 0x00454e8f) `PUSH 0x0`. The only allocator that does
not hardcode it is `AddWaypoint` (slot 90), which forwards its own argument, and slot 90 is reachable
only through the seven client/executor vtables — i.e. from the wire. **So no waypoint the shipped game
creates is ever retained on arrival: the retain path exists and is unreachable in practice.** Not
dead *code* — a dead *branch*, and the name describes what the reader does with it rather than a
feature anyone can trigger.

---

## 20. Five wire-reachable sites dispatch a vtable slot on an unchecked id (CONFIRMED)

**The primary primitive is not the crash. It is unbounded remote executor-thread ESP drift.**

Five sites take an actor/unit id straight off the wire, resolve it, and call a fixed vtable slot
index on the result. Three are `Actor` slots in the executor's `ExecutorThreadProc` @ 0x00509050;
two are `Unit` slots in the client's `ApplyUpdateMessage` @ 0x004fde70, and because `GetUnitById`
returns a `Unit`, the `Actor` slot numbering does **not** apply to those two.

| Site | Dispatcher | Insn | Tree / slot | Triggered by | Arg bytes pushed |
|---|---|---|---|---|---|
| 0x00509eba | `ExecutorThreadProc` | `CALL [EDX+0x184]` | `Actor` 97 | commands 0x0a / 0x0c | 0x10 |
| 0x00509fe4 | `ExecutorThreadProc` | `CALL [ESI+0x180]` | `Actor` 96 | commands 0x0b / 0x0d | 0x10 |
| 0x0050a03f | `ExecutorThreadProc` | `CALL [EDX+0x188]` | `Actor` 98 | command 0x0e | 0x04 |
| 0x004fdf16 | `ApplyUpdateMessage` | `CALL [EDX+0x188]` | `Unit` 98 | update 0x39 | 0x0c |
| 0x004fe957 | `ApplyUpdateMessage` | `CALL [EDX+0x180]` | `Unit` 96 | update 0x1e | 0x08 |

Slots 96/97/98 exist on `CharacterActor` and its descendants. They do **not** exist on
`PickupActor` (86 slots), `TrackObjectActor`, `TumbleweedActor`, `BackgroundCreatureActor`,
`BlockerActor` (83 each), `ProjectileActor` (85), base `MobileActor` (95) or `PresidentActor` (96,
so 97 and 98 are past the end) — see `actor_vtable_notes.md`.

### The drift, which is the part that matters

An out-of-range slot index most often lands on a **getter with a bare `RET`** — the `.rdata`
following these tables is full of them. The arm pushed 0x4-0x10 bytes of arguments and does not
clean them, because every one of these arms assumes a **callee** pop (`RET n`), which is what the
in-range callee would have done. So each malformed message leaks 4-16 bytes of the executor
thread's stack, without bound. A peer looping command **0x0e** at any pickup id is the cheapest
version: 4 bytes per message, one message.

This is exactly the failure mode CLAUDE.md's `RET`-form trap describes, and it presents the same
way: a delayed, non-deterministic access violation with **EIP on the stack** and a faulting module
of "unknown", nowhere near the culprit, appearing only once something has run often enough. It will
be mis-blamed on GkPlus's hooks — hence this entry.

### Nothing constrains it

- **No class check on the lookup.** `GetActorById` @ 0x0044e0b0 and `GetUnitById` @ 0x0044e070 are
  identical short separate-chaining hash walks matching `[node+0xc] == id`. Neither inspects a
  vptr, a size, or an RTTI predicate.
- **No sender-ownership check** in either dispatcher.
- **Ids are guessable.** `NextActorId` @ 0x007b9ffc (named `num_actors` until this pass — it never
  decrements) is read-then-`INC`ed at every `CreateActor` / `SpawnProjectileActor` arm, reset to 0
  by `FUN_0052dcb0`, and saved and restored with the game. The id space is therefore dense and
  monotonic: a peer needs neither an information leak nor a lucky guess.
- **Every class is addressable.** All sixteen `Actor` classes self-register in the actors hash from
  the **base** `Actor` constructor, so no subclass is out of reach of an id.
- **`Unit+0x127` is not a guard.** It is `user_placed_by_camera_track`, a base-`Unit` flag set only
  by `CameraTrackObject_SetUserPlacement` @ 0x004dcc79 and cleared by `CameraTrackObject_Stop`
  @ 0x004dd3d9, and 0 on every ordinary unit. (The `0x120` sometimes cited alongside it is base
  *`Actor`*, in the other class tree; base `Unit` is 0x130.)

### The crash half, stated no more strongly than it is

Where the index lands past the end of `.rdata`'s vtable run entirely, it reads whatever follows.
For `PresidentActor` (vtable 0x00669380, 96 slots, ending at 0x00669500 where the string pool
begins with `"nanofrag_projectile"`) slots 96/97/98 read the ASCII dwords `"nano"`, `"frag"` and
`"_pro"`. None is a mapped address, so this is an immediate AV at a **fixed,
non-attacker-chosen EIP**: a remote crash, **not** control-flow hijack. Do not overstate it — the
drift above is the more serious of the two.

### Why this was previously filed as "no evidence"

The selection path looked like it might constrain which classes an order could name. It does not,
and the reason is worth recording: update **0xc3** (arm 0x00500ade) inserts an arbitrary
wire-supplied unit id into the local client's `SelectedUnits` @ 0x007b46d8 via `AddToSelection`
@ 0x0049ed30 — a bare hash insert, no type filter. The real filter is at *order-issue* time, and it
is **not** the RTTI ladder: `IssueGroundTargetOrderToSelection` @ 0x0049f010 gates on `Unit`
**slot 11**, which is `XOR EAX,EAX; RET` in every class except the five `CharacterUnit`-family
ones, where it is `MOV EAX,[ECX+0x290]; RET` — i.e. a *controllable* flag, not `IsCharacter`. That
is why the defect has no in-play symptom: ordinary play never produces these messages for a class
that lacks the slot. It says nothing about what a peer can send.

### Measurement notes

Both dispatchers' jump tables are resolved and no override is needed: `ApplyUpdateMessage` is
18,501 bytes with **0 undefined bytes** and `ExecutorThreadProc` 10,732 with 0. So the negative
half of this finding is not the "sweep over `.text` is a statement about the disassembly" trap.

---

## 21. Both render-queue entry points turn an allocation failure into a null dereference (CONFIRMED)

**Status:** latent — unreachable unless the pool allocator is exhausted. Recorded because the code
*reads like a guard*, so the next person to look at it will believe the failure is handled.

`RenderQueue_Submit` @ 0x0059d760 pool-allocates the 0x30-byte `DrawItem` and then tests it:

```
0059d77a  TEST ESI,ESI
0059d77c  JZ  0x0059d78d
0059d77e  MOV dword ptr [ESI + 0x4],0x1      ; refcount = 1   (the success path)
0059d785  MOV dword ptr [ESI],0x66da04       ; vptr
0059d78b  JMP 0x0059d78f
0059d78d  XOR ESI,ESI                        ; <-- the "failure" path
0059d78f  MOV EAX,dword ptr [EBP + 0x8]
0059d792  MOV dword ptr [ESI + 0xc],EAX      ; <-- unconditional write through ESI
```

The null check exists, branches, and lands **four bytes before an unconditional store through the
pointer it just proved was null**. All it actually skips is the vptr and refcount initialisation;
the eight field writes that follow (0x0059d792 through 0x0059d7bc) run either way. So on
allocation failure this faults at 0x0059d792 writing address 0xc, rather than dropping the draw.

`RenderQueue_Add` @ 0x005a8eb0 has the identical shape at `XOR ESI,ESI` @ 0x005a903f.

Two things make this worth a section rather than a footnote. It is **the shape of a bail without
the effect of one**, which is the kind of thing a reader credits without checking — the branch
target is right there. And a fault at 0x0059d792 would present as an access violation writing a
tiny address from inside the renderer, with the real cause (an exhausted pool) several layers away
and no diagnostic anywhere; compare §11, where the pool allocator's own critical section is
disarmed by a flag nothing sets.

Neither is reachable in practice: `pool_alloc` failing at all means the page sub-allocator is out,
and nothing in a normal session gets close. GkPlus's `gk::SubmitDrawItem` is unaffected — it fails
closed on its own arguments before it reaches either function.

---

## Dead code: things that look reachable and are not

Not defects. Recorded so nobody analyses them twice.

- **`GameState` 2, 3, 4, 9 and 20 are all unreachable, and 14 has no writer at all.** This entry
  used to cover 4 and 20 and to rest on "the functions which would set them have zero xrefs". The
  verdict holds, the **evidence is now positive rather than an absence**, and the **scope is wider**.

  `WinMain` @ 0x0046aef0 ends in a 20-entry jump table at **0x0046bec8** indexed by `GameState - 1`
  (`MOV EAX,[GameState 0x006b02b4]; DEC EAX; CMP EAX,0x13; JA <loop>;
  JMP dword [EAX*4 + 0x0046bec8]`). There are **three** such dispatchers, all indexed the same way:
  that one, the per-frame draw dispatcher `FUN_0046a710` (byte index table @ 0x0046aab0 into targets
  @ 0x0046aa84, 11 arms over states 1-19, so **state 20 is out of its range entirely**), and
  `HandleKeyPress2`'s @ 0x0046f2f0.

  Six states are **static-picture screens** sharing one image global `PictureScreenImage`
  @ 0x007ba3c0 and one deadline global `PictureScreenDeadline` @ 0x006b0298:

  | state | setter | image | frame handler | draw handler | key handler | setter reachable |
  |---|---|---|---|---|---|---|
  | 1 | `EnterTitleScreen` 0x0056dc50 | `bitmaps\Title.rim` | `RunTitleScreenFrame` 0x0046edd0 | `DrawTitleScreen` 0x0056e2f0 | `HandleTitleScreenKeyPress` 0x00470e00 | **yes** (4 refs) |
  | 2 | `Enter2000ADSplashScreen` 0x0056def0 | `bitmaps\2000AD splash screen.rim` | `Run2000ADSplashFrame` 0x0046cc80 | `Draw2000ADSplashScreen` 0x0056e030 | `Handle2000ADSplashKeyPress` 0x00470e80 | no — **transitively** |
  | 3 | `EnterBlankPictureScreen` 0x0056e080 | (none, 60 s deadline) | `RunBlankPictureScreenFrame` 0x0046ec50 | `DrawBlankPictureScreen` 0x0056e2d0 | `HandleBlankPictureScreenKeyPress` 0x00470e40 | no — 0 refs |
  | 4 | `EnterRebellionLogoScreen` 0x0056e350 | `bitmaps\REBLOGO.rim` | `RunRebellionLogoScreenFrame` 0x0046e540 | `DrawRebellionLogoScreen` 0x0056e470 | `HandleRebellionLogoScreenKeyPress` 0x0046f6c0 | no — 0 refs |
  | 9 | `EnterHiscoreScreen` 0x0056e4c0 | `bitmaps\Hiscore.bmp` | `RunHiscoreScreenFrame` 0x0046e4f0 | `DrawHiscoreScreen` 0x0056e5e0 | 0x0046f291 | no — 0 refs |
  | 20 | `EnterSplashScreen` 0x0056dd90 | `bitmaps\splashscreen.rim` | `RunSplashScreenFrame` 0x0046d0f0 | (none) | `HandleSplashScreenKeyPress` 0x0046f020 | no — 0 refs |

  **State 2 is dead transitively**, which is the part a per-function xref count could never show:
  `Enter2000ADSplashScreen` has exactly two callers, 0x0046e571 and 0x0046f6f8, and *both* sit inside
  the **state-4** handlers. So the original boot sequence was REBLOGO (4) -> 2000AD splash (2) ->
  Title (1), and only Title survived.

  **The cause**: `WinMain`'s first screen action is `PlayFmvAndSetState(10 /* Logos.bik */)`
  @ 0x0046bc81. The FMV chain replaced the static logo screens — states 10-13 are `Logos.bik`,
  `Intro_FMV.bik`, `Outro_FMV.bik`, `Fail Out final.bik`, driven by `PlayFmvAndSetState` @ 0x004b0570
  (`void __fastcall(uint state, char set_video_mode)`) and framed by `RunFmvStateFrame` @ 0x0046d2e0.

  **Nothing can reintroduce a dead value.** `PlayFmvAndSetState` writes `GameState = param_1` and
  dispatches through a **4-entry** table @ 0x004b0724 on `param_1 - 10`, so any other value is a wild
  jump — and all four of its call sites pass a literal 10, 11, 12 or 13. The only three *variable*
  writers of `GameState` are likewise closed: `WinMain` @ 0x0046bd48 (`7 -> 5`, via `CMOVZ`),
  `MenuLoadGame` @ 0x004e6d0a (`0x11` or `0x12` only), and `FUN_004b0a30` @ 0x004b0a7a-0x004b0aef
  (saves the value, writes 0xf, restores it).

  **Also dead, as a consequence:** `Run2000ADSplashFrame` @ 0x0046cc80, the four frame handlers for
  states 3/4/9/20, the four key handlers, and all five draw handlers.
- **`MobileActor::AddWalkingSpeed` @ 0x00539ed0 is dead, and so is update `0x5f`.** It adds a delta
  into `MobileActor+0x178`, calls `UpdateSpeedAndTurnRadius` and broadcasts `0x5f` (60 bytes,
  reliable). It has no callers, and a byte scan of the whole 0x00600000+ range for its
  little-endian address finds no pointer either, so it is not a vtable slot: **the shipped binary
  never sends update `0x5f`.**
- **`File_OpenForRead` @ 0x005e25f0 and `File_OpenForWrite` @ 0x005e2750 have zero references
  image-wide.** Two complete `CreateFileA` wrappers (`GENERIC_READ`/`OPEN_EXISTING` and
  `GENERIC_WRITE`/`CREATE_ALWAYS`) that nothing calls - worth knowing before treating them as part
  of the file-I/O surface a mod filesystem has to cover (`file_io_notes.md`).
- **Command `0x29` is dead on both ends.** Its sender @ 0x004fcdf0 has no call sites anywhere, and
  `ExecutorThreadProc` maps id `0x29` to the default arm of both its if-chain and its jump table.
  Its payload word 2 is never initialised either, which stays academic while nothing sends it.

**"No xrefs" is not sufficient evidence of death**, and this pass proved it twice.
`BuildModemCompoundAddress` @ 0x00512080 was recorded as dead on exactly that basis and **is not**:
its address sits in a 2-slot vtable at 0x0066742c, found only by a byte scan for the little-endian
dword, because the referencing data had never been defined. The image-codec registry
(`file_io_notes.md`) was the other case - seven callers, all tail-jump thunks sitting in
undisassembled bytes, reported as zero. Before calling anything dead, scan `.rdata`/`.data` for the
address as raw bytes and look for a thunk that jumps to it. A reference count is a fact about the
database, not about the binary.

A third case has since been added: **`IsWithinElevationLimit` @ 0x005420a0** was recorded in
`ai_behaviour_notes.md` as having no xrefs at all and possibly dead, and has **two** call sites
(0x00452ff8, 0x004535b7) inside `AiThink_Bot` - both in bytes that were undefined because 8,400
bytes of that function sat behind unresolved jump tables. The same undefined region hid the only
write of `Actor::alert_state = 1`, which the same file had concluded was never written. The rule
generalises past xref counts: **a sweep over `.text` is a statement about the disassembly**, so
before asserting "nothing writes X" or "nothing calls Y", check how much of the region the sweep
covered is actually defined.

**And here is the negative control that makes the rule usable.** The `GameState` pass above is the
first dead-code verdict in this file that ran the sweep *and* recorded that the sweep can fire, which
is what makes it different in kind from the four that collapsed:

- `GameState` @ 0x006b02b4 has a **complete, enumerable writer set**. A byte scan for its
  little-endian address over 0x00400000-0x0083ba00 returned **90 hits, 21 of them writes, all
  accounted for** — including two that sat in undefined bytes and had to be hand-decoded
  (0x0044c0a0 `A1 B4 02 6B 00` = a *read*, in what is now `CommandInfoDialogTest` @ 0x0044c0a0, the
  console command **"INFO DIALOG TEST"**; and 0x004b067e `89 3D B4 02 6B 00` = the write inside
  `PlayFmvAndSetState`'s arms).

  **Both undefined regions have since been recovered, and the census now reconciles exactly**: 90
  byte occurrences and 90 *defined* references — 69 reads and 21 writes, with 0x004b067e among the
  writers. Two corrections to the paragraph as it originally stood:

  - **It said 22 writes. The snapshot supports 21** (20 defined references at the time, plus the one
    hidden write), and there is no read-modify-write to explain the difference. The verdict is
    unaffected — a census claiming *more* writers than exist is conservative and cannot have missed
    a state's writer — but 22 should not be quoted onward unchecked.
  - **It said elsewhere in this file that `PlayFmvAndSetState`'s arms were "now disassembled". They
    were not.** The function was named, but its body ended at its unresolved indirect jump
    (0x004b0570-0x004b064f) and the four switch arms plus jump table (0x004b0650-0x004b073f, 240
    bytes) were still undefined data — so the *write* at 0x004b067e, the more dangerous of the two
    occurrences, stayed invisible to any reference-based sweep. Recovering it needed a **jump-table
    override**, not just `COMPUTED_JUMP` references: the selector is `LEA EAX,[EDI-0xa]` @ 0x004b0644
    with **no bound check**, which is exactly the case the standing trap says the decompiler
    re-derives for itself. Measured cascade for that recovery: +2 functions, -310 undefined `.text`
    bytes. `CommandInfoDialogTest` cost +1 function and -79 bytes, with `GameState` writes unchanged
    — which is the check that its recovery could not have disturbed the verdict.

  A second scan for
  rebased `[reg+disp]` forms over bases 0x006b02a4-0x006b02b7 found only those same two sites, and
  the global's **address is never taken** (no `LEA`, no `PUSH`). So there is no route to it that a
  reference count would miss.
- For each of the two setters, three independent negatives: zero Ghidra references; zero occurrences
  of the little-endian address anywhere in initialized memory (so **not** a vtable slot or
  function-pointer table entry — the trap that resurrected `BuildModemCompoundAddress`); and **zero
  `E8`/`E9` rel32 whose target is either address across all 7,128 undefined ranges / 111,809
  undefined bytes of `.text`** (982 rel32 candidates examined, ±4-byte margin) — the trap that
  resurrected the image-codec registry.
- **The rel32 sweep demonstrably fires.** The same scan found three genuinely hidden call edges that
  no database reference showed: 0x0046cba0 `CALL PlayFmvAndSetState` and 0x0046cbbb / 0x0046cc12
  `CALL EnterTitleScreen`, all inside `RunFmvStateFrame`'s undisassembled arms at
  0x0046cb92-0x0046cc7b. A sweep that finds nothing is only evidence if you have shown it finds
  something.

The generalisation: **report what your sweep would have caught, not just what it caught.** A negative
control turns "I found no callers" into "I found no callers with an instrument I proved works here".

---

## Debugging Gunlok: what actually works

- **cdb is at** `C:\Users\franc\AppData\Roaming\Binary Ninja\dbgeng\Windows Kits\10\Debuggers\x86\cdb.exe`.
- **`cppvsdbg` (VS Code) uses `vsdbg.exe`** from the cpptools extension. It speaks
  DAP on stdio and the handshake works, but it enforces a licence check restricting
  it to VS Code / Visual Studio as the host and aborts the session for anything
  else. Usable by pressing F5; not scriptable.
- **`WinDbgX.exe`'s command-line launch does not work** in this environment (it
  fails on `notepad.exe` too, so it is not Gunlok-specific).
- **`bp d3d8+0x...` silently resolves wrong**: cdb parses `d3d8` as the hex literal
  `0xD3D8`, because all four characters are hex digits. Use `module!symbol` form,
  or breakpoint the *caller* in `gl.exe` — `Gl` parses fine as a module name.
- **cdb will not load our clang-built PDB** (`bm d3d8!*Foo*` reports "No matching
  code symbols found") even with `-y` pointing at `build/Debug` and `.reload /f`.
  `llvm-symbolizer --obj=build/Debug/d3d8.dll --relative-address <rva>` resolves
  our frames perfectly, so symbolize offsets that way instead.
- **Do not breakpoint `Gl+0x6e498`** (`RunGameFrame`'s pump call) during play. It
  is gated on `NumCommandsToExecute != 0`, which *stays* non-zero for the whole
  duration of a pending `WAIT`, so it fires every frame and makes the game
  unplayable.
- **cdb echoes its entire `-c` command list on startup.** Any `.echo MARKER` you
  add appears twice: once in that echo and once as real output. Anchor greps with
  `^MARKER$` or you will parse the echo and conclude a fault occurred on every
  clean run.
- For crash-detection soaks, prefer running **without** a debugger and reading
  WER's Application Error log plus the exit code; a clean exit writes
  `scripts\GLkeys.cfg`. Undebugged the game loads a level and exits in ~22s; under
  cdb with the warning redirect on it could not finish the parse in 40s.
- **WER already writes full dumps, and `cdb -z` on one beats every live attach.**
  Crashes land in `%LOCALAPPDATA%\CrashDumps\gl.exe.<pid>.dmp` (~6 MB) with no
  configuration needed. `cdb -z <dump> -c ".ecxr; k 40; q"` gives a complete stack;
  **attaching to the live process does not** - `-pv` (non-invasive) reports "Could
  not fetch any stack frames" for the faulting thread, and an invasive `-p` attach
  breaks in on its own thread, reports "Unable to get initial context information"
  for `k`, and killed the target on `qd` twice out of two. Symbolize our frames from
  the RVAs the stack prints (`d3d8+0x3c25d`) with
  `llvm-symbolizer --obj=build/Debug/d3d8.dll --relative-address 0x3c25d`. That took
  a `make.role` fault from "the game stopped responding" to
  `MakeRoleJs -> MakeRole (MakeRole.cpp:447) -> HierarchyResolveNamedPointPos ->
  ___ascii_stricmp` in one command.
- **"Not responding" is not evidence of a spin, and neither is high CPU.** Gunlok
  burns a full core at the front end normally, so a climbing `Process.CPU` says
  nothing. The fault above presented as a hang - `Responding: False`, CPU rising,
  main thread parked in `ZwWaitForMultipleObjects` - and was an access violation
  all along, with that wait being WER's own error reporting. Read the Application
  Error log before concluding anything about a wedged process.
- **The REPL is a debugger you already have.** Most of this session's findings came
  from `GKPLUS_REPL_PORT=9222` plus a 30-line TCP client, not from cdb: it reads
  live game state (`roles.count`, `[...roles].map(r => r.name)`), and a binding that
  crashes the game names itself, because the snippet that stopped answering is the
  one that did it. Note the failure mode, though - **a crash looks like a socket
  timeout**, so check the process afterwards rather than trusting the timeout.
