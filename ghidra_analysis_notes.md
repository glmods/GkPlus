# Reverse-engineering traps and Ghidra mechanics

The accumulated traps behind every measurement in `address_map.md` and the `*_notes.md` set,
plus the mechanics of driving Ghidra through the Jython MCP. **This is reference — look it up
when doing binary work, rather than carrying it.** It was a section of `CLAUDE.md` until that
file outgrew the context it is loaded into; the *policy* half (who may write to the database,
and the renaming convention) stays there under "Ghidra Database Hygiene".

Read "Analysis Traps" before concluding anything negative about the binary — that a field has
no writer, a function no caller, or a signature no arguments. Almost every entry below is a
case where such a conclusion was drawn and was wrong.

## Analysis Traps

- `StartExecutorThread` -> `ExecutorThreadProc` is a `CreateThread` **entry-point reference, not a
  call edge** — leave it in a caller-closure and every executor-only function falsely appears
  reachable from the main thread. Cut it; treat thread procs as roots.
- "No xrefs" often means the *referencing data was never defined* (vtables sitting as raw bytes).
  Scan for the little-endian pointer before concluding a virtual has no callers.
- **A sweep over `.text` is a statement about the disassembly, not about the binary.** The sibling
  of the trap above: a sweep for writes to a field, or for calls to a function, only ever sees what
  is *disassembled*. Two measured cases, both inside `AiThink_Bot`, whose 8,400 bytes sat outside
  the function body behind four unresolved jump tables:
  `ai_behaviour_notes.md` concluded `alert_state = 1` has no writer anywhere and the manual's orange
  cone is therefore unreachable — the write is at 0x00453e3f, in bytes that were undefined at the
  time; and `IsWithinElevationLimit` @ 0x005420a0 was recorded as having no xrefs at all and
  possibly dead — it has two call sites, both in the same undefined region. So before concluding a
  field has no writer or a function no caller, check that the *containing* functions are fully
  disassembled — in practice, that no function in the area has undiscovered bytes inside its
  address range.
- **Defining the referencing data is sufficient to recover the code — and you cannot opt out.**
  Measured: defining an undefined pointer table also **disassembles its targets**. Across 360 runs
  / 3,394 entries, with **no `DisassembleCommand` run and no function created**, 1,866 of 1,868
  distinct targets became instructions, the function count rose 12,826 -> 12,845 and the `.text`
  undefined-byte census fell by 32,427 — Ghidra's auto-analysis acts on the new reference.
  `AutoAnalysisManager.setIgnoreChanges(True)` was tested and **does not suppress it**. So a
  two-phase plan ("define the tables now, disassemble later") does not exist; budget the analysis
  into the define step.
- **The decompiler does not consult `COMPUTED_JUMP` references.** Also measured. Creating correct
  `COMPUTED_JUMP` references at an unresolved switch site makes the *listing* and the function body
  right — edge counts, body size and the byte census all reconcile — but the decompiler
  **re-derives the jump table itself** and ignores those references. Where it cannot, it emits
  `Could not recover jumptable at <site>. Too many branches` and **`Treating indirect jump as
  call`**, which also starves any downstream switch in the same function, whose blocks are then
  never reached by the CFG. Two consequences: `getJumpTables()` returning 0 is **not** evidence
  that the references are wrong; and the fix is a **jump-table override**,
  `ghidra.program.model.pcode.JumpTable(site, dests, True).writeOverride(func)`. In this binary
  that was needed at the three sites with **no bound check** on the index (0x00563c67, 0x00498185,
  0x004524b1) — a switch with a `CMP`+`JA` bound decompiled correctly from the references alone.
  Applying the override to the `InGameMenu` outer switch also unlocked three inner tables the
  decompiler then recovered unaided. Note too that `getCases()` may report fewer cases than the
  table has slots when two indices share a body; the override's persistent artifact still stores
  each `case_N`, so that is render-time coalescing, not data loss.
- **4-aligned is not sufficient to call a dword a pointer.** A 16-bit array whose value is >= 0x0063
  in both halves reads as a valid `.text` pointer: the worked case is 0x006a4b70-0x006a4b97, a run
  of the constant 0x0063 where every dword reads `0x00630063`. **The "require distinct values" test
  this bullet used to prescribe is measured insufficient and has been replaced**: ten runs deferred
  as suspected switch tables all had distinct values and passed it anyway, and every one turned out
  to be an interior slice of the GLS parser's 16-bit yacc tables (`gls_system_notes.md` has the map).
  The worked case above is not an isolated curiosity either — it is a slice of `GshParse_yytable`,
  where 0x0063 is parser state 99 repeated. Two tests that do work:
  1. **Byte shape** — reject a dword whose bytes are `XX 00 YY 00` with both `< 0x80`. A genuine
     `.text` pointer here (0x00401000-0x0064cfff) always has a nonzero byte[1] or byte[2]; a pair of
     small `short`s never does.
  2. **Decisive and cheap: require the candidate base to appear as a `disp32` somewhere in `.text`.**
     A jump table nothing dispatches through is not a jump table. This is immune to the
     undisassembled-code trap, because a dispatch *must* encode its base in the `JMP` or a `LEA`
     whether or not that code is defined — so it settles the question without needing the referencing
     code recovered first.
  Two cheaper tells, both free: MSVC **never puts a jump table in a writable section**, so a
  candidate in `.data` is already refuted; and a run whose targets span **more than one function** is
  not a single switch. Low distinctness alone
  is normal inside a vtable region, where shared base slots and `__purecall` repeat legitimately.
  And **`.reloc` cannot be used as the absolute-vs-RVA oracle in this database**: all 4,884 recorded
  relocations are confined to `.text` 0x00400000-0x0043ffff, with none in `.rdata`/`.data`, so "this
  dword has no relocation" carries no information.
- **A `CMP <global>,[TLS+0x20]` followed by `JG` to an out-of-line block is an MSVC thread-safe-static
  guard, not domain data.** `[TLS+0x20]` is `_Init_thread_epoch`. The tell is the triple
  `_Init_thread_header` @ 0x005e459e / `_atexit(<empty RET stub>)` / `_Init_thread_footer`
  @ 0x005e4554, and **the real static is the dword *before* the guard**. This cost two independent
  sessions: 0x007b48bc/c4/cc were read as "camera-transition deadlines against the thread tick" and
  are guards, while the actual statics at 0x007b48b8/c0/c8 are the saved recon camera roll/pitch/yaw.
  Reading a guard as a domain value invents a quantity that does not exist.
- **An MSVC `__finally` funclet has two entry points, and the scope table names the wrong one for
  Ghidra's purposes.** `_EH4_SCOPETABLE_RECORD.HandlerFunc` points at a 1-4 instruction
  enclosing-frame register reload that **falls through** into a body the parent also `CALL`s a few
  bytes later — and Ghidra has already made a function at *that* inner address. So "the scope table
  points at an address with no function" is **not** a missing function: `createFunction` there yields
  a stub that falls through into a foreign body. Rename the inner function and *label* the scope-table
  address. Measured at 56 of 72 `__finally` records here. The sibling shape is the `__except`
  handler — a bare `MOV ESP,[EBP-0x18]` rejoining the parent's `__try` resume point — which must
  never become a function at all. Generalises past the CRT and fails silently.
- **A Function ID "Library Function - Single Match" can be flatly wrong**, which is a different trap
  from the multi-match hole below. FID matches on body *shape*, and the MSVC scalar deleting
  destructor is four instructions of boilerplate around `CALL <dtor>` / `operator delete(this, N)` —
  generic enough that it filed two RIF chunk destructors under
  `std::basic_stringbuf<...>::'scalar deleting destructor'`. The cheap test is the **reference set**:
  a real library function is reached from library code, and these were each reached from exactly one
  chunk vtable slot. The `operator delete` size is the second test and doubles as a free `sizeof`
  measurement (`(this, 0xac)` pinned `Object_Chunk`, `(this, 0x44)` pinned two others).
- **A receiver's provenance must be read at the dispatch site, not inferred from what an honest
  sender does.** An executor arm was framed for a whole prior attempt as taking its receiver from the
  client's selection; the arm actually opens `MOV ECX,[EBX+0x4]` / `CALL GetActorById`, an id straight
  off the wire, and the selection constrains only the client. Related tell: **`GetUnitById` vs
  `GetActorById` decides which of the two class trees a slot number belongs to** — a `Unit` receiver
  makes the `Actor` slot counts inapplicable.
- Reachability and gate counts must be **transitive**: `CommandSpawn` looks ungated but delegates
  to `DoSpawn`, which holds the gate. Converges around depth 2.
- **Reverse basic-block reachability is worthless inside a dispatch loop — use dominators.** Every
  arm of `ExecutorThreadProc`'s command switch ends in a `JMP` back to the loop head at 0x00509150,
  so the back edge makes *every* arm appear to reach *every* tail block, and a "which arm can get
  here?" query returns all of them. That is what made the `AssignToTeamSlot` /
  `BroadcastStopAtPosition` question look malformed for a whole prior attempt: both sites are in the
  executor's **periodic tick** (entered on a `WaitForMultipleObjects` timeout or a null dequeue, not
  from the switch at all), and reverse reachability could not distinguish that from "reached by
  every command". Ask which blocks **dominate** the site instead. The same shape appears in any
  message pump, `WndProc` or `AiThink_*` tick loop.
- **A jump table settles which ids reach an arm, and the answer can be a defect.** Decode the table
  rather than reasoning from the arm's own compares: `ExecutorThreadProc`'s byte-index table
  @ 0x0050bae0 into targets @ 0x0050ba3c (index `id - 4`, bounded `CMP EAX,0x39`) shows arm
  0x00509e14 is reached by **exactly** ids 0x1d and 0x1f — which is what proves its
  `CMP EDI,0x20 / SETZ CL` can never fire (`game_defects_notes.md` §18). An arm's compare against
  a constant is only meaningful once you know the id set that reaches it.
- Read **disassembly** for computed sizes/arguments — the decompiler folds constants differently
  (`iVar + 0x48` vs the actual `LEA EDX,[EDI + 0x49]`).
- When extracting call arguments in bulk, take literals from the **same source line** as the
  buffer variable; pairing P-code operands positionally desyncs in multi-call-site functions.
- Bulk-recovering broadcast ids: scan each vtable slot body for the last `MOV dword [EBP+x], imm`
  before a `CALL BroadcastToPlayers` (0x00504bf0). Single-candidate sites are trustworthy — the
  scan independently reproduced every id already documented — but sites yielding 2+ candidates, or
  none, are computed (`0x41 + close_range`) and need disassembly.
- Verify against the Ghidra DB, not the `*_notes.md` — a claim in the notes is only as good as
  the measurement behind it, and not every one was measured.
- A **mistyped global pointer** makes the decompiler emit `Global[n].field_0xNN` shorthand whose
  real offset is `n * sizeof(wrong type) + 0xNN`. Retype the global before reading anything
  through it, and distrust notes written in `[n].field_` form.
- When sweeping accesses to a global struct, track the **offset carried in the register**
  (propagate through `MOV`/`LEA`/`ADD`/`SUB`, kill on other writes and on `CALL` for EAX/ECX/EDX).
  A plain `[reg+disp]` scan misses every field reached after an `ADD reg,imm` rebase. Sanity-check
  that all recovered offsets land inside the known struct size.
- Existing **names in the DB are not evidence either** — several shipped names described
  something the function does not do. Confirm a name against the body before building on it,
  and rename when it's wrong.
- A wrong name does not stay local. `save_system_notes.md` described the savegame `kind` field as
  "0 = player character" — inferred from a constructor name that does not exist in the binary —
  which made the format look impossible for any actor larger than 0x178. `kind` is just
  `IsProjectile()`. When a note calls something contradictory or broken, suspect the label first.
- A stub that decompiles to `return;` is **not necessarily a no-op**: check for `RET 0xN`. A
  non-zero operand means the function takes `N` bytes of stack arguments and discards them, which
  usually makes it a *setter* whose base implementation ignores the value. `Actor` slots 9 and 54
  were documented as per-tick callbacks for exactly this reason; both are `RET 0x4` and pair with
  getter slots 8 and 30.
- **`RET` vs `RET n` is the ground truth for a calling convention, and Ghidra's label is not.**
  A function with stack arguments ending in a bare `RET` is caller-clean (`__cdecl`); `RET n` is
  callee-clean (`__stdcall`, or `__thiscall`/`__fastcall` with stack args past the registers). The
  DB had `pool_free` @ 0x005715b0 as `__stdcall` when it ends in a bare `RET` at 0x0057166f and
  every game call site does `CALL free` … `ADD ESP,0x4`. `src/Memory.cpp` believed the label.
  **Getting this wrong does not fail where you can see it**: calling a `__cdecl` function through a
  `__stdcall` pointer leaks 4 bytes of stack per call (the compiler emits `sub esp,4` to undo a
  callee pop that never happens), so ESP drifts until some *later* frame's epilogue returns to
  garbage. It presents as a non-deterministic access violation with **EIP on the stack** and a
  faulting module of "unknown", nowhere near the real culprit, and it stays dormant until something
  calls the function often. Cross-check the convention of every wrapped function against its `RET`,
  and treat "it worked so far" as meaning "nothing called it in a loop yet".

  The same test applies to **arity**, not just convention, and it is worth running in bulk: `RET n`
  states exactly how many bytes of arguments a `__thiscall` callee pops, so a declaration with the
  wrong parameter count drifts ESP by the difference. Sweeping all sixteen Actor vtables (1,460 slot
  entries) against `src/Actors.h` found **ten wrong declarations** — see `console_command_notes.md`
  §6.3 for the table and the two false-positive sources (slot 0's hidden destructor flag, and a
  by-value `Vec3` being 12 bytes). All ten are fixed and the sweep is **clean at 1,460/1,460**, and
  it is a check that can fail: restoring the pre-fix declarations makes it flag exactly those ten
  slots. Re-run it after adding any slot.

  `RET n` gives you the argument *bytes*, not the parameter count or their shapes — a `Vec3*`
  plus three dwords and a by-value `Vec3` plus a `bool` are both 0x10. What discloses a by-value
  class parameter is the **callee**: MSVC emits `_eh_vector_destructor_iterator_` on it, which is
  why `MobileActor` slot 86's stub has a body at all. Slot 90 has the same `RET 0x10`, takes a
  pointer, and dereferences it.
- The decompiler's `Class::Method` header line does **not** always match
  `FunctionManager.getParentNamespace()`. Query the namespace; never read ownership off the C output.
- For a vtable slot, the owning class is the **shallowest** class whose vtable contains that
  address — not the most-derived one that inherits it. 55 of 249 Actor-family functions were filed
  under a descendant. Fixing `setParentNamespace` also repairs the `this` parameter type for free.
- **There are two parallel class trees, one per thread** - the executor's `Actor` family and the
  client's `Unit` family - and a size, an offset or a slot index is only comparable *within* one.
  `role_system_notes.md`'s "MobileActor 0x230" and `level_loading_notes.md` §7's "Mine 0x238" row
  describe different objects and are both right. Slot 35 (`GetSize`) is the size oracle in **both**
  trees. The two trees are *structural mirrors* - same sixteen class names, same edges - and the
  15-wide RTTI predicate ladder in slots 36-50 maps index-for-index across them, but nothing else
  does: client `PresidentUnit` is 0x248 against `PresidentActor` 0x240. Note also that the 0x238
  client class is **`MobileUnit`, not a mine class**: `ai mine` lands there only because a mine
  carries no weapon (`character->weapon == 0x21`), and it shares the class with every other unarmed
  character. `rendering_notes.md` §5.1 is the client tree.
- Vtable **slot indices are branch-local**. Two classes deriving from a common base number their own
  extension slots from the same index, so a "rename slot N everywhere" sweep silently clobbers an
  unrelated method in a sibling branch (`PickupActor` slot 85 vs `MobileActor` slot 85).
- The **last** vtable in an adjacent run has no successor to bound it. `PresidentActor`'s
  (0x00669380) runs to the string pool at 0x00669500 — 96 slots, not the 84 that "ends at the next
  vtable" implies. Bound the final table with the reference test, never with adjacency.
- A `ParsedThingBase` subclass may be **larger than 0x1b60**: check the `malloc` size in its
  `DoParseXxx`. `ParsedMap` is 0x1b78 - the extra 0x18 is the placed-object binding hash.
- A **`ToXxx` converter that only default-initialises a field can never tell you what it
  means** - constants are not evidence. When a struct is mostly `field0xNN`, check whether the
  only function with the type applied is the converter; if so the analysis simply never reached
  a consumer. `ParticleGenerator` sat that way until `ParticleEmitter_Ctor` was found.
- **Same size is not the same type.** `ParticleTypeInfos`' elements are 0xd4 bytes, exactly
  `sizeof(ParticleGenerator)`, and are reached through the same code - but their element ctor
  builds `Vec3`s at completely different offsets. Confirm with the ctor/dtor pair, not the size.
- A struct copied with **`MOVUPS`/`MOVQ` shows you its real record boundaries**. Five
  `ParticleGenerator` sub-records looked like `{vec4, int}` preceded by padding until the
  16-byte load from `+0x2c` (not `+0x30`) proved each record starts one dword earlier.
- A **table of N filled instances beats any single decompilation**. Diffing the 13 cases of
  `InitParticleTypeInfo` down a column named most of `ParticleTypeInfo` in one pass: the field
  that is 9.81 for snow/rain/sparks is gravity, the one that is 30 for rain and 9 for snow is
  fall speed. When a struct resists, look for the initialiser that fills every variant.
- **A shared epilogue after a switch is usually derived state.** Four of `ParticleTypeInfo`'s
  `Vec3`s are just the others divided by the tick rate; naming them as independent fields would
  have invented four physics parameters that do not exist.
- A helper reached only through subsystem X is **not necessarily X's**. `ParticleTypeInfo`'s
  `render_state` ctor/finalise looked particle-specific until a caller check showed the shadow
  renderer and five other subsystems using them. Check callers before baking a prefix into a name.
- **A register argument spilled in the prologue reads as an uninitialized local.** `LoadLevel`
  was documented as `StdCall<void>` because its `bool` parameter is `MOV [EBP-0x175],CL` at the
  third instruction: the decompiler shows a `local_179` with no assignment anywhere and every
  P-code def marked `INDIRECT`, i.e. "some call might have written it". A local that is *read*
  several times and *never written* is an argument until proven otherwise — check the first
  handful of instructions for a store from ECX/EDX, then check what each call site puts there
  (here `MOV CL,1` vs `XOR CL,CL`, which turned out to be new-level vs savegame-restore).
- **A hooked function declared `void` that actually returns a value fails only in the build you
  do not ship.** The same `LoadLevel` @ 0x004e0980 was hooked as `void(__fastcall)(char)` by
  `LoadScreenSystem`. It returns a status, `LoadGame` @ 0x00505730 tests it, and a `void` hook
  therefore returned whatever happened to be in EAX after its own body: **non-zero in
  RelWithDebInfo, zero in Debug**. So restoring a savegame dumped the player back to the main
  menu at the end of the load, in Debug only, silently - every failure path in that loader is
  `CloseHandle` / `ResumeExecutor` / return with no message. A fresh level start ignores the
  result, so only savegames broke, and only in the configuration CLAUDE.md recommends for hook
  work. Two rules follow: a wrapper's return type is as load-bearing as its arguments and needs
  the same `RET`-form scrutiny, and **forward the return as `int`** so all 32 bits of EAX pass
  through exactly as the callee left them - a narrower type lets the compiler extend or truncate,
  handing the caller a value the unhooked game never would. If a hook is right in one config and
  wrong in another, suspect EAX before suspecting timing.
- **No `static_assert` means nothing is pinning the layout.** Before trusting a GkPlus struct
  mirror, check it actually has one - `ParticleGenerator` and `Projectile` had none despite
  this file claiming otherwise.
- **A signature census goes stale the moment a consolidator saves, and it goes stale in both
  directions.** Re-derived from the live DB, an `undefined`-return population recorded as 5,909 in a
  CSV measured **6,031**: 53 CSV rows were no longer `undefined`, **175 undefined functions were
  absent from the CSV**, and at least one *name* was stale (0x00466b90 is
  `SetCurrentDirectoryToGameRoot` in the DB and `SetCurrentDirectory` in the CSV). One of a round's
  four "crisp defects" - `AiThink_Pathfinder` @ 0x004556d0 - was already `USER_DEFINED` and correct.
  **Rank and name from the live database, not from a saved census file**, and treat a census as a
  work-list rather than as evidence.
- **Ghidra's Function ID analyzer leaves a signature-shaped hole that is not a defect.** When several
  library functions share a base name - C++ template instantiations are the usual case - FID applies
  the base name only and writes the candidate prototypes into a **plate comment**, leaving
  `signatureSource == DEFAULT` and zero parameters. "0 parameters" then means *no prototype*, not a
  wrong one. The rule: FID plate comment says *Multiple Matches With Same Base Name* **and**
  `signatureSource == DEFAULT` → exclude. **609 functions binary-wide** match it; 27 of them also
  have a `RET n`, which is what made them look like arity contradictions. They are UCRT
  `__crt_stdio_output::output_processor<...>` instantiations, and their true prototype is already in
  each plate comment. Never `setPlateComment` on one - it overwrites silently and that comment is
  the only record of the candidate set.
- **A byte-purge sweep cannot see a missing register argument.** A bare `RET` is equally consistent
  with "N register arguments", "N stack arguments `__cdecl`", and "zero arguments", so comparing
  declared argument bytes against the `RET` operand rules out ESP drift and rules out nothing else.
  Two measured instances: `CommandBatchAndBroadcast` @ 0x00448400 and `CommandVulnerability`
  @ 0x0044a600 take **no arguments at all** (they read the console word buffer from the global
  0x006af5f8) while GkPlus declares both `FastCall<void, int, char *>` - ABI-harmless, but the
  forwarded `length`/`args` are garbage; and `LightSet_Ctor` @ 0x00579a20 is `__fastcall` with
  **zero** declared parameters, so neither its `this` nor its return exists in the decompiler's
  output. Add a "does the entry read ECX/EDX before writing them" column to any such sweep.
- **Four decompiler/model artifacts that each flipped a return-type verdict.** A **tail `JMP` is
  recorded as a call reference**, so the instructions after it are unrelated code. **The shape of
  that artifact is narrower than this bullet used to claim, and two of its three named functions
  were wrong**: it is an **incoming** tail `JMP`, not an outgoing one, so it afflicts a callee whose
  *callers* tail-jump to it. `PlayUiSound` had exactly 2 such references out of 88, and
  `RenderQueue_Submit` has **zero** among its 102 — neither is a tail-call pass-through at all.
  (`ConsolePrint` is unchecked; leave that clause alone.) The sharp rule: **filter reference sites by
  the mnemonic at the *from*-address**, not by the callee's own epilogue. And prefer the positive
  test where one exists — *a function whose EAX at `RET` differs between two paths by construction
  is `void`*, which is what settled `RenderQueue_Submit` (one path leaves `RenderQueue_Add`'s return,
  the other a destructor's `this`). That needs no caller census and cannot be faked by an artifact. A
  last-EAX-write walk must **skip `__security_check_cookie` @ 0x005e46aa**, which preserves EAX -
  that alone flipped three functions from "void" to their real return. `FILD` counts as reading ST0
  in Ghidra's model (41 fake floating-point consumers on one function). `OR EAX,0xffffffff`
  reads-and-writes (3 fake readers).
- **Return-type WIDTH is a separate claim from the return type.** A function ending `MOV AL,1` or
  `MOV AL,BL` leaves EAX[31:8] as residue from a preceding call, so it is `bool`/`char` and **not**
  `int` - measured at 0x00466b80, 0x00466b90, 0x004e0980, and at Actor slots 57/58, where all eight
  dispatch sites read only AL. The inverse holds for a *hook*: GkPlus must forward all 32 bits of
  EAX, so a wrapper's return type stays `int` precisely because the game's is narrower. Both halves
  are needed; either alone misleads.

## Ghidra MCP Mechanics

- **A Ghidra agent must not spawn subagents of its own.** The pool is 20 and nested fan-out
  saturates it; the failure surfaces as `Concurrent subagent limit reached` on an unrelated agent.
  Both agent definitions say so; a brief that invites one to "delegate the rest" undoes it.
- There are **two kinds of context** and picking the wrong one is not a style choice.
  `create_readonly_context` binds an immutable snapshot and any write in it fails with
  `Transaction not permitted: read-only`; `create_context` is read-write and wraps modifications in
  a transaction for you. Every explorer in a round shares one snapshot, so they cannot disagree
  because the program moved under one of them — and that snapshot is taken from the program **as
  last saved in Ghidra**.
- **A read-only snapshot can be STALE EVEN AFTER a successful `save_program`, so never use one to
  verify your own or an agent's work.** Measured the hard way: a consolidator applied ~45 renames,
  four new classes and several new types and reported `save_program` returning `saved: true`; a
  `create_readonly_context` taken *afterwards* still showed every one of them as `FUN_`, with **no**
  unsaved-changes warning to signal that the view was old. That read produced a confident and
  completely wrong conclusion that the agent had fabricated its report — followed by a redundant
  re-application that created a duplicate data type and needed its own recovery. **A read-write
  context binds the live program and is the authoritative read.** So the rule is narrower than the
  bullet below suggests: snapshots are for *exploring*, and anything you intend to assert about
  current DB state — especially a negative, "this was not applied" — must come from
  `create_context`. Corollary: when two readings disagree, suspect the instrument before concluding
  against the agent.
- **A report saying "not yet applied" is a statement about when it was written, not about the
  database.** The same staleness as the census bullet above, one level up. Read the DB before
  applying a findings report; it costs a handful of read-only calls and prevents redundant or
  conflicting writes.
- **A consolidator's edits are invisible to every explorer spawned afterwards until the program is
  saved**, and this is measured, not inferred: a consolidator wrote a plate comment, and the next
  `create_readonly_context` came back with `The program has unsaved changes in Ghidra. This snapshot
  reflects the last saved state, so those changes are not visible here.` So an explorer in a later
  round reports the *old* name for something already renamed. **The plugin's `save_program` is the
  fix** — it is File > Save from inside the agent, so the cycle is: read from snapshots, write from
  one read-write context, `save_program`, then take fresh snapshots to see the result. **End every
  consolidator pass with it.** It is only on the consolidator's allowlist, which is the point.
  Before that tool existed the save had to be done by hand in the GUI, and the cost was real: one
  session needed three manual saves, and a repo pass that could not open a context wrote ~45
  function names taken from explorer recommendations rather than from confirmed reads — several of
  which the consolidators had deliberately deviated from — which then needed a whole audit round to
  find. Treat that warning in a context's response as the signal that a round of consolidation has
  not been saved yet, and remember that the reports, not the database, are the record of what a
  round found.
- `execute_command` runs in a **persistent** Jython context — globals survive between calls, so
  accumulate into a global and process in batches. Always `close_context` when done; for a read-only
  context that is what lets the shared snapshot be freed.
- **A Java exception does not derive from Jython's `Exception`, so `except Exception, e` does not
  catch one** — it propagates, fails the whole `execute_command` call, and discards every `print`
  after the `try`. So one bad element mid-batch loses the rest of that call's output, exactly like a
  timeout. Measured on a refused write (`db.NoTransactionException` out of `DBHandle.checkTransaction`
  escaping the handler). Use a bare `except:` or `except java.lang.Exception` around anything that
  can throw from the Ghidra API — a `setName` that collides, a `replaceAtOffset` on a bad offset, a
  read of uninitialized `.bss` — and keep the `DONE` set updated as you go.
- 30 s timeout: >~15 decompilations per call times out. Batch with a `DONE` set so a timeout
  doesn't lose progress.
- `mem.getBytes()` into a Jython `bytearray` does not marshal back (silently returns zeros) — use
  `getInt()`/`getByte()` or `jarray`.
- `createLabel` replaces a dynamic `DAT_` symbol, deleting any `Symbol` handle fetched beforehand
  (`ConcurrentModificationException`) — re-fetch after.
- `findDataTypes` may return a pre-existing duplicate from another category; consolidate with
  `dtm.replaceDataType(old, new, False)` instead of leaving two definitions.
- Editing a struct field: `setFieldName` on an `undefined` component **silently does not persist**,
  and renaming a component *wider* than the field mislabels its neighbours (`MobileActor+0x187` was
  a 4-byte `int` spanning 0x188). Use `clearAtOffset` then
  `replaceAtOffset(off, dt, dt.getLength(), name, comment)`, and re-check `getLength()` afterwards —
  an unchanged struct size is the guard that the edit landed where you meant. **That guard earns its
  keep**: skipping the `clearAtOffset` on a non-packed structure makes `replaceAtOffset` *insert*
  rather than overwrite, and a 0x2c struct silently became 0x4c. The recovery is **not**
  `deleteAtOffset` in a loop — that does not shrink a non-packed structure and will hang the 30-second
  call — it is `deleteAll()` followed by sequential `add()`, which respects alignment.
- **Creating a data type without checking for an existing one produces a `.conflict` twin, and the
  cleanup is easy to get wrong.** `dtm.findDataTypes(name, list)` matches on *display* name, so a
  search for `"Foo.conflict"` can return the canonical `Foo` — removing that result deletes the real
  type and collapses every parameter using it to `undefined`. Resolve a duplicate with
  `dtm.replaceDataType(dup, orig, False)` per the note below, or check `dtm.getDataType(path)` for the
  exact path first; and note the path may not be the one you assume (`/gunlok/ResourceStringEntry`,
  not `/ResourceStringEntry`), so a path-based "it is absent" check is itself a false-negative risk.
- For `__thiscall`, the `this` type comes from the function's **parent class namespace**
  (`setParentNamespace`), not `updateFunction`; a parameter literally named `this` binds to ECX.
- Ghidra's `__thiscall` puts **only** `this` in ECX, everything else on the stack. A function
  taking args in ECX *and* EDX is `__fastcall` — model it that way and check
  `getVariableStorage()` on each param afterwards. `updateFunction("__thiscall", ...)` also
  auto-inserts its own `this`, shifting your explicit params by one.
- **`setPlateComment` overwrites silently.** Two consolidator passes destroyed a pre-existing plate
  comment before the pattern was caught; one was recoverable only from an earlier session's report,
  and had to be restored marked as a reconstruction. Read comment type 3
  (`CodeUnit.PLATE_COMMENT`) first and append after a separator rather than writing over it. The
  same applies to `setComment` on any comment type.
- After renaming in Ghidra, `grep` the `*.md` files for the old `FUN_`/`DAT_` name.
- A big function's decompilation can exceed the MCP result token cap (it auto-saves to a
  `tool-results` file needing chunked reads). Instead write it to the scratchpad via Jython
  (`open(p,'w').write(r.getDecompiledFunction().getC())`) then `Read` it — or hand that file to
  a subagent to summarize so a 1000+-line function never enters the main context.
- `getInt()`/`getBytes()` on an uninitialized `.data`/`.bss` global throws `MemoryAccessException`
  (zero-init globals aren't in the file image) — read meaning from writers/disassembly, not live bytes.
- `getInstructionAt` returns None for an address inside an instruction — use
  `getInstructionContaining` and walk `getPrevious()` to dump a window around a data reference.
- Walk to a vtable slot's body with `mem.getInt(vtbl + slot*4)`, masking `& 0xffffffff` (Jython ints
  are signed, so a high address comes back negative and `getAddress` rejects it).
- **Do not scrape constants out of the decompiler's local variables.** It reuses one local for
  several `.rdata` symbols, so a regex over `x = DAT_...;` then `field = x;` silently attributes the
  *last* binding to every use. Scan the disassembly instead, tracking register loads. For the GLS
  section constructors the pattern is SSE and two encodings matter: `MOVSD xmm,[const]` +
  `MOVSD [reg+disp],xmm` writes one 8-byte slot, while `MOVAPS xmm,[const]` + `MOVUPS [reg+disp],xmm`
  writes **two adjacent slots** from a 16-byte constant. Integers and booleans are plain
  `MOV byte/dword ptr [reg+disp], imm`. `XORPS xmm,xmm` then a store means zero.
