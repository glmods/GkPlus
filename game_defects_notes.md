# Game defects (Gunlok's own, not GkPlus's)

Bugs that live in `gl.exe` and reproduce without GkPlus. Recorded here so a later
session can decide whether the mod should paper over them, and so nobody spends a
second evening blaming our hooks for them.

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

### Where to start if someone picks this up

WER already wrote the dump; `cdb -z <dump> -c ".ecxr; k 40; q"` plus
`llvm-symbolizer` on the `d3d8+0x...` RVAs is the recipe below. The dumps from the
session that found this are gone (WER keeps a bounded ring), so reproduce first.

---

## 5. `PolygonAdjacencyTest` @ 0x0048ecf0 overflows a 3-element buffer on degenerate level geometry

**Status:** open, unpatched in the game; **worked around in the Blender exporter**
(`shapes.welds_degenerate`), which drops the geometry that triggers it.

**Severity:** fatal, and *diagnostically* nasty — it is a `/GS` fast-fail, so
running under a debugger suppresses the WER dump and the fault names neither the
asset nor the polygon.

### Mechanism

`PolygonAdjacencyTest` is polygon vtable **slot 0x50**, `__thiscall(this, other)`,
"do these two polygons share an edge?". It walks `this`'s three vertex pointers,
scans `other`'s vertex-pointer array for each, and appends every match — **by
pointer identity** — to a local `Vec3[3]`:

```c
_eh_vector_constructor_iterator_(&shared, 0xc, 3, ...);   // 3 x 12 bytes, at EBP-0x38
...
dst = (undefined4 *)((int)&shared + count * 0xc);         // no bound on `count`
if (v == other_verts[i]) { count++; dst[0] = ...; dst[1] = ...; dst[2] = ...; }
```

It returns true when exactly **2** matched — a shared edge — after which it
projects the edge and validates the connection.

There is no capacity check. `shared` is at `EBP-0x38` and the function's `/GS`
cookie is at `EBP-0x14`, so the **fourth** match writes its first dword exactly
onto the cookie:

```
-0x38 + 3 * 0xc = -0x14
```

The epilogue's `__security_check_cookie` then fast-fails with
`STATUS_STACK_BUFFER_OVERRUN` (**0xc0000409**, subcode 0x2
`FAST_FAIL_STACK_COOKIE_CHECK_FAILURE`) — *not* an access violation.

### What produces four matches

Matches are `sum over this's 3 corners of (occurrences of that corner in other)`.
With `this` non-degenerate the total can never exceed 3, because `other` has only
three slots. So the overflow requires **`this` to carry a repeated vertex pointer**
*and* `other` to repeat a vertex `this` also has — i.e. two polygons that are both
degenerate once the loader has welded vertex records by position, meeting in the
same section-grid cell. Observed peak was **9**.

A triangle becomes degenerate at the weld when two of its corners land on the same
position. `SHPRAWVT` is **integer**, so this includes corners that were distinct in
the authoring tool and collapsed on quantization — which is how an exporter creates
them without any modelling error.

### Call path

```
ToMap+0x2587
  LoadOrBuildSectionAdjacency+0x307        @ 0x0044fef0 — the .cut sidecar builder
    BuildPolygonAdjacencyGrid+0x137        @ 0x0048aa00 — triple-nested grid loop
      PolygonAdjacencyTest+0x87d           @ 0x0048ecf0 — slot 0x50
        __security_check_cookie → __report_gsfailure
```

`BuildPolygonAdjacencyGrid` walks the level's 3D section grid (dimensions at
`+0x6c`/`+0x68`/`+0x64`, cells at `+0x34`), and for each polygon rescans the 26
neighbouring cells, calling slot 0x50 on each candidate pair and linking accepted
ones through slots 0x58 / 0x5c.

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

`PolygonAdjacencyTest` @ 0x0048ecf0 and `BuildPolygonAdjacencyGrid` @ 0x0048aa00
were `FUN_`-named; both now carry plate comments with the above. `RET`-form and
arity were not re-derived — the calling convention comes from the call site at
0x0048ab35 (`MOV ECX,EBX` / `PUSH EDI` / `CALL [EAX+0x50]`).

---

## 6. `Role::interface_beam_script` is shared by pointer to every actor, and freed per actor

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

## 7. `Font_QueueText` @ 0x005782e0 dereferences null when its node allocation fails

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

## 8. `ToRole` @ 0x0047cc20 dereferences a null `Hierarchy` when a role's `.rif` is missing

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

## 6. Ambient sound volume (`V` in a `DUMOBJTX`) is parsed and then discarded

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
