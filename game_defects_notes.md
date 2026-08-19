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
| 3 | `GetResourceString` walks its table with no terminator | a missing localized string id |
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

## 3. `GetResourceString` @ 0x00579000 walks its table with no terminator

The localized string lookup is a five-instruction linear scan with **no end check**:

```
00579000  MOV EAX,dword ptr [ECX]        ; the table head
00579002  CMP dword ptr [EAX],EDX        ; is this the id?
00579004  JZ  0x0057900d
00579006  ADD EAX,0x14                   ; next 0x14-byte entry
00579009  CMP dword ptr [EAX],EDX        ; <-- faults here
0057900b  JNZ 0x00579006                 ; ... forever
0057900d  MOV ECX,dword ptr [EAX + 0x4]
00579010  TEST ECX,ECX
00579012  MOV EAX,0x7c14b4               ; a default string when the entry is null
00579017  CMOVNZ EAX,ECX
```

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

## 16. `AiBeginInvestigate` @ 0x0045e050 sets its deadline with no clock read (PROPOSED)

**Status:** proposed, not settled. The mechanism is measured; the interpretation rests on one
global's meaning.

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

so `+0x90` becomes `[0x007c07dc] + seconds * tick_rate` rather than `now + seconds * tick_rate` -
a deadline in the distant past if 0x007c07dc holds the ticks-per-second constant it appears to,
which would make the investigate state expire on the tick it is entered.

**What would settle it** is the writer of 0x007c07dc: if that global is a per-thread copy of the
*current* time, the code is correct and merely reads oddly. Nothing in this pass identified the
writer, so the mark stays PROPOSED - keep it that way until someone reads it.

---

## Dead code: things that look reachable and are not

Not defects. Recorded so nobody analyses them twice.

- **`GameState` 4 and 20 are unreachable.** `WinMain` @ 0x0046aef0 ends in a 20-entry jump table at
  **0x0046bec8** indexed by `GameState - 1` (`MOV EAX,[GameState 0x006b02b4]; DEC EAX;
  CMP EAX,0x13; JA <loop>; JMP dword [EAX*4 + 0x0046bec8]`). A sweep of every writer of `GameState`
  shows that the functions which would set 4 and 20 have **zero xrefs of any kind**, so those two
  arms never run - which makes `RunSplashScreenFrame` @ 0x0046d0f0 (state 20) and
  `HandleSplashScreenKeyPress` @ 0x0046f020 dead.
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
