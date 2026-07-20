# Gunlok Trigger System - Reverse Engineering Notes

## Overview

The trigger system uses a global doubly-linked list of trigger nodes. Each trigger has
a type, coordinates, timing/radius data, a list of actor token names, and a script
filename. Triggers are evaluated every game tick by EvaluateTriggers (FUN_0050ccc0).

## Global State

- `FirstTrigger` @ `0x006af858` - Trigger* sentinel node for the global trigger list
- `NumTriggers` @ `0x006af85c` - int, count of active triggers
- `PTR_006af860` @ `0x006af860` - void*, cached array (freed on list changes)
- `DAT_006af864` @ `0x006af864` - int, cache valid flag

## Data Structures

### TriggerBase (0x0C = 12 bytes) - Doubly-linked list node
```
0x00: TriggerBaseVtbl* vtbl    // vtable pointer (@ 0x00652078)
0x04: TriggerBase*     next
0x08: TriggerBase*     prev
```
The sentinel node points to itself (next=prev=self) when the list is empty.

### ActorNameNode (0x10 = 16 bytes) - String-bearing linked list node
```
0x00: TriggerBase base         // inherits linked list node (vtbl @ TriggerVtbl 0x0065207c)
0x0C: char*       actor_name   // heap-allocated string (actor token name or other string)
```
Used as nodes in TriggerList. The field (Ghidra label `actor_name`) stores **actor token
names** when embedded in TriggerData's target list at 0x44. It is a generic string whose
semantics depend on context. Distinct from the `Trigger` node below, which is the real
global-list node.

### Trigger (0x10 = 16 bytes) - Global trigger linked list node
```
0x00: TriggerBase   base      // inherits linked list node (vtbl @ TriggerVtbl2 0x00652094)
0x0C: TriggerData*  data      // pointer to the 0x68-byte trigger data struct
```
These nodes form the global trigger list anchored at FirstTrigger. (Formerly named
`Trigger2`.)

### TriggerList (0x10 = 16 bytes) - Container for a list of Trigger nodes
```
0x00: TriggerBase* sentinel       // sentinel node (allocated, self-referencing when empty)
0x04: int          count          // number of Trigger nodes
0x08: void*        cached_array   // cached array pointer, freed on modification
0x0C: int          cache_flag     // set to 0 on modification
```

### TriggerData (0x68 = 104 bytes) - Main trigger data
```
0x00: TriggerKind  trigger_kind         // enum value identifying trigger type
0x04: Vec3f        coords[4]            // up to 4 coordinate sets (48 bytes)
      [0] = 0x04  primary position
      [1] = 0x10  secondary position (doors, shot, defog, instantdisplace)
      [2] = 0x1C  (four_doors)
      [3] = 0x28  (four_doors)
0x34: int          unused_0x34          // never accessed in trigger eval. Padding.
0x38: int64        time_or_radius       // overloaded meaning:
                                        //   TIME/TIME_IF_ALIVE/FRAG_SCORE/TIME_LIMIT: deadline tick
                                        //   SHOT: creation tick
                                        //   LOCATION/DOOR/etc: radius as int64
0x40: float        radius_squared       // param^2 as float (for distance checks)
0x44: TriggerBase* target_list_sentinel // embedded TriggerList: sentinel for actor name list
0x48: int          target_list_count    // embedded TriggerList: count of actor names
0x4C: void*        target_list_cache    // embedded TriggerList: cached array pointer
0x50: int          target_list_cache_flag // embedded TriggerList: cache valid flag
0x54: char*        script_name          // strdup'd script filename to execute on trigger fire
                                        // (role name for SHOT, NULL for DOOR/DEFOG/INSTANTDEATH)
0x58: int          team_or_warning      // overloaded by trigger type:
                                        //   FRAG_SCORE: team_id (1=Goodie, 2=Baddie)
                                        //   TIME_LIMIT: one-minute-warning flag (0=not shown, 1=shown)
                                        //   others: always 0 from console
0x5C: byte         armed                // 1=ready to fire, 0=already fired (resets when
                                        // condition becomes false again). Prevents repeated
                                        // firing while condition remains true.
      byte[3]      padding_0x5d         // 3 bytes padding (writes as char, not int)
0x60: int          last_trigger_actor   // actor ID that last activated this trigger.
                                        // -1 = none. When trigger resets (condition false),
                                        // sends event 0x52 with this actor ID then resets to ready.
0x64: int          unused_0x64          // never accessed in trigger eval. Padding.
```

**CRITICAL: Field semantics at 0x44 and 0x54**

The embedded TriggerList at offset 0x44 stores **actor token names**, NOT script
filenames. EvaluateTriggers iterates this list and calls `GetTokenValue` on each
entry to resolve actor token names to actor IDs. The resolved IDs are used to:
- Check if actors are alive/dead (DEATH, TIME_IF_ALIVE)
- Get actor positions for distance checks (LOCATION_SPECIFIED, ESCORT, PROXIMITY)
- Toggle door actors (DOOR, DOOR_ONCE, DOORS_EITHER)
- Check alert/attack state (BEEN_ALERTED, BEING_ATTACKED)

The field at offset 0x54 stores the **script filename** that is passed to
`QueueScriptExecution` when the trigger condition is met. This is confirmed by
the trigger fire handler at 0x0051066d:
```c
if (triggerData->script_name != NULL) {
    QueueScriptExecution();
}
```

> **Multiplayer:** only the **filename** is authoritative, never the file contents.
> `QueueScriptExecution` @ 0x00505080 pushes the name onto the host's local `ScriptQueue`
> *and* broadcasts it as update `0x67`, so every joining client runs `ExecuteCommandFile`
> on its **own local copy** of that `.gcs`. Triggers themselves still only evaluate on the
> host (`EvaluateTriggers` runs on the executor thread, which joiners never start), but the
> resulting script runs everywhere. Clients whose `Scripts\` directory differs from the
> host's will diverge — bounded by the `IsExecutorRunning()` gate in the `Command*` handlers,
> which no-op on a joiner. See `directplay_protocol_notes.md` §8.11 and
> `threading_model_notes.md`.

**Evidence for field assignments:**
- `trigger_kind` at 0x00: CommandRemoveTrigger compares `piVar2[0]` with 0x10 (TRIGGER_SHOT)
- `coords` at 0x04: `_eh_vector_destructor_iterator_(piVar2 + 1, 0xc, 4, ...)` in CommandRemoveTrigger
- `target list` at 0x44: `TriggerList::DeleteTriggers(piVar2 + 0x11)` in CommandRemoveTrigger
- `script_name` at 0x54: `free(piVar2[0x15])` in CommandRemoveTrigger
- Total size: `Dealloc_(piVar2, 0x68)` in CommandRemoveTrigger
- `armed` at 0x5C: trigger eval checks `[0x5c] != '\0'`, sets to `'\0'` on fire, `'\x01'` on reset
- `last_trigger_actor` at 0x60: set from actor field on trigger fire, checked for -1 on reset
- `team_or_warning` at 0x58: compared to 1/2 for "Goodie"/"Baddie" in FRAG_SCORE case,
  compared to 0 and set to 1 for one-minute warning in TIME_LIMIT case

### TriggerKind enum (4 bytes, unsigned)
```
 0 = TRIGGER_DEATH               // fires when actors in target list die
 1 = TRIGGER_LOCATION            // fires when any actor enters radius around coords[0]
 2 = TRIGGER_LOCATION_SPECIFIED  // fires when actors in target list enter radius
 3 = TRIGGER_LOCATION_ALL        // fires when ALL actors are in radius
 4 = TRIGGER_LOCATION_TIMED      // like LOCATION but with time component
 5 = TRIGGER_INSTANTDEATH        // instantly kills actors entering radius
 6 = TRIGGER_INSTANTDISPLACE     // teleports actors from coords[0] to coords[1]
 7 = TRIGGER_TIME                // fires after delay (converted to absolute deadline)
 8 = TRIGGER_ESCORT              // fires when actors in target list enter radius
 9 = TRIGGER_PROXIMITY           // fires when actor in target list is near another actor
10 = TRIGGER_DOOR                // toggles door actors when player enters radius (repeatable)
11 = TRIGGER_DOOR_ONCE           // like DOOR but only fires once
12 = TRIGGER_DOORS_EITHER        // two-position door trigger
13 = TRIGGER_FOUR_DOORS          // four-position door trigger
14 = TRIGGER_LIGHT_UP            // activates lighting when actor enters radius
15 = TRIGGER_DEFOG               // removes fog of war in an area
16 = TRIGGER_SHOT                // fires after countdown, related to shooting/role
17 = TRIGGER_BEING_ATTACKED      // fires when actors in target list are being attacked
18 = TRIGGER_FRAG_SCORE          // deathmatch: displays team score after time expires
19 = TRIGGER_TIME_LIMIT          // game time limit: ends game when deadline reached,
                                 // shows "one minute left" warning
20 = TRIGGER_TIME_IF_ALIVE       // like TIME but only fires if actors in target list are alive
21 = TRIGGER_BEEN_ALERTED        // fires when actors in target list have been alerted
```

Note: TRIGGER_DOOR (10) and TRIGGER_DOORS_EITHER (12) share evaluation code in EvaluateTriggers.

## Target list usage by trigger type

| Trigger type       | Target list (0x44)           | script_name (0x54)    |
|--------------------|------------------------------|-----------------------|
| DEATH              | actor names to watch for death | script to run       |
| LOCATION           | (empty/"" entry)             | script to run         |
| LOCATION_SPECIFIED | actor names to check position | script to run        |
| LOCATION_ALL       | (empty/"" entry)             | script to run         |
| LOCATION_TIMED     | (empty/"" entry)             | script to run         |
| INSTANTDEATH       | (empty/"" entry)             | NULL                  |
| INSTANTDISPLACE    | (empty/"" entry)             | NULL                  |
| TIME               | (empty/"" entry)             | script to run         |
| ESCORT             | actor names to check position | script to run        |
| PROXIMITY          | reference actor name (1 entry) | script to run       |
| DOOR               | door actor names to toggle   | NULL                  |
| DOOR_ONCE          | door actor names to toggle   | NULL                  |
| DOORS_EITHER       | door actor names to toggle   | NULL                  |
| FOUR_DOORS         | (empty/"" entry)             | script to run         |
| LIGHT_UP           | (empty/"" entry)             | script to run         |
| DEFOG              | (empty/"" entry)             | NULL                  |
| SHOT               | (empty/"" entry)             | role name             |
| BEING_ATTACKED     | actor names to monitor       | script to run         |
| FRAG_SCORE         | (programmatic)               | (programmatic)        |
| TIME_LIMIT         | (programmatic)               | (programmatic)        |
| TIME_IF_ALIVE      | actor names to check alive   | script to run         |
| BEEN_ALERTED       | actor names to monitor       | script to run         |

## Key Functions

### AddTriggerToGlobalList @ 0x0043e240
Creates a new trigger and adds it to the global list. Prototype:

```c
void AddTriggerToGlobalList(TriggerKind kind, Vec3f *coords, long long time_param,
                            TriggerList targets, char *script, int team)
```

- `targets` is passed by value (compiler places 4 struct fields directly on stack)
- `script` is strdup'd into TriggerData.script_name (0x54)
- `team` is stored at TriggerData.team_or_warning (0x58)

For TIME and TIME_IF_ALIVE triggers, `field_0x38` is converted to an absolute deadline:
  `deadline = current_time + ticks_per_unit * delay`

For SHOT triggers, `field_0x38` is set to the current time (creation timestamp).

### EvaluateTriggers @ 0x0050ccc0
Main trigger evaluation function, called every game tick. Iterates the global
trigger list and processes each trigger based on its type (big switch statement).
Contains all 22 trigger type handlers.

When a trigger condition is met, execution reaches LAB_0051066d which:
1. Checks if `script_name` (0x54) is non-NULL
2. If so, calls `QueueScriptExecution()` to run the script
3. Calls `RemoveTriggerFromGlobalList()` to remove the fired trigger

### CommandAddTrigger @ 0x00443070
Console command handler. Parses: `add trigger <TYPE> [args...]`

**Position arguments** accept either raw `X Y Z` floats or a **dummy actor name**
(resolved via `ConsoleParsePosition` which looks up the entity's world position).
Script files reference dummies extensively: `add trigger location trigduma 9 junksilo_a.gcs`

**Script command prefixes** (discovered from .gcs files):
- `@add trigger ...` — self-referencing (script re-registers itself)
- `easy/medium/hard add trigger ...` — difficulty-dependent registration
- `if (var = val) add trigger ...` — conditional registration

Console syntax per trigger type (with examples from game scripts):

**Script-first triggers** (first word after fixed params → 0x54, remaining words → 0x44 list):

- `DEATH <script> <actor1> [actor2...]`
  ex: `add trigger death crtbaa.gcs tbaa`
- `LOCATION_SPECIFIED <pos|dummy> <radius> <script> <actor1> [actor2...]`
  ex: `add trigger location_specified debug 3 cyberbay.gcs gunlok`
- `ESCORT <pos|dummy> <radius> <script> <actor1> [actor2...]`
  ex: `add trigger escort next 2 junkyard_cutscene.gcs hark`
- `BEING_ATTACKED <script> <actor1> [actor2...]`
  ex: `add trigger being_attacked junksiloalert_a.gcs archore_b`
- `TIME_IF_ALIVE <delay> <script> <actor1> [actor2...]`
  ex: `add trigger time_if_alive 10 l03guardat.gcs baddieza`
- `BEEN_ALERTED <script> <actor1> [actor2...]`
  ex: `add trigger been_alerted l03guarda.gcs baddieza`

**Script-only triggers** (single script name → 0x54, empty list at 0x44):

- `TIME <delay> <script>`
  ex: `add trigger time 10 crdoorabc.gcs`
- `LOCATION <pos|dummy> <radius> <script>`
  ex: `add trigger location trigduma 9 junksilo_a.gcs`
- `LOCATION_TIMED <pos|dummy> <radius> <script>`
  ex: `add trigger location_timed 0 -30 -36 2 switches_pressed_a.gcs`
- `LOCATION_ALL <pos|dummy> <radius> <script>`
  ex: `add trigger location_all ENDDUM 20 s3_end.gcs`
- `LIGHT_UP <pos|dummy> <radius> <script>` (default radius=1)
  ex: `add trigger light_up debug 3 debugging_complete.gcs`
- `FOUR_DOORS <pos1> <pos2> <pos3> <pos4> <radius> <script>`
  ex: `add trigger four_doors -29.12 2.10 29.75 ... 2 L12_end_level.gcs`

**Actor-list-only triggers** (all words → 0x44 list, 0x54 = NULL):

- `DOOR <pos|dummy> <radius> <door_actor1> [door_actor2...]` (default radius=2)
  ex: `add trigger door -37.3 -0.88 -62.5 10 gate`
  Note: copies coords[0] into coords[1] (single activation zone)
- `DOOR_ONCE <pos|dummy> <radius> <door_actor1> [door_actor2...]` (default radius=2)
  ex: `add trigger door_once -44 2.5 -83 6 mudslidelift_a`
  Note: copies coords[0] into coords[1], fires only once
- `DOORS <pos1|dummy1> <pos2|dummy2> <radius> <door_actor1> [door_actor2...]`
  ex: `add trigger doors -35.8 -12 0 35.8 -12 0 2 dishlift_a`
  Note: creates TRIGGER_DOOR (kind=10) with two separate coord sets
- `DOORS_EITHER <pos1|dummy1> <pos2|dummy2> <radius> <door_actor1> [door_actor2...]`
  ex: `add trigger doors_either carlifta carliftb 2 carlift`

**Special ordering** (actor before script in console args):

- `PROXIMITY <radius> <actor_name> <script>` (default radius=2)
  ex: `add trigger proximity 3 frend L4_frend_activated.gcs`
  (one actor in target list, script parsed as separate word)

**No script or target list**:

- `INSTANTDEATH <pos|dummy> <radius>`
- `INSTANTDISPLACE <pos|dummy> <radius> <dest_pos|dummy>`
  ex: `add trigger instantdisplace deatha 2 lemminga`
- `DEFOG <defog_pos|dummy> <defog_radius> [<trigger_pos|dummy> <trigger_radius>]`
  ex: `add trigger defog defogtriga 10 defoga 35`
  Note: parses defog center into coords[1] first, then optional trigger area into
  coords[0]. If trigger position omitted, uses defog position. Fills coords[2] with
  trigger_radius as float (defog area extents). Default radii=10.

**Other**:

- `SHOT <start_pos|dummy> <end_pos|dummy> <count> <role_name>` (default count=8)
  ex: `add trigger shot laseraa laserab 5 mattslaser`
  role_name → 0x54, validated via GetRoleByName but return value is discarded (bug?).
  Empty list at 0x44.

Note: FRAG_SCORE, TIME_LIMIT are NOT available as console commands.

### CommandRemoveTrigger @ 0x00444500
Only supports removing SHOT triggers by coordinate match.

### TriggerList::Ctor @ 0x0044ca10
Allocates sentinel, initializes empty list.

### TriggerList::CtorWithScript (FUN_0044c900) @ 0x0044c900
Constructor + CreateTrigger in one call. Initializes list then adds one entry.
Name is misleading - it adds a generic string entry, not specifically a script.

### TriggerList::CreateTrigger @ 0x0044e8c0
Adds a new Trigger node (with string) to the list. Despite the name, the string
stored may be an actor token name rather than a script filename.

### TriggerList::DeleteTriggers @ 0x0044ce40
Removes all triggers from list and frees the sentinel.

### TriggerList::RemoveFirst (Dtor) @ 0x0044eb60
Removes the first (most recently added) trigger from the list.

### TriggerList::CopyList @ 0x0044c950
Copies a trigger list into the outgoing parameter area on the stack. Used to pass
a TriggerList by value as parameter to AddTriggerToGlobalList.

### ConsoleParsePosition @ 0x0044ece0

Resolves a position from the console command line. Accepts either:

1. Three floats (X Y Z) parsed as literal coordinates
2. A named dummy/actor whose position is looked up by iterating scene entities
   (case-insensitive `lstrcmpiA` comparison, skips "Sound" entities)

If `out_rotation` (param_2) is non-NULL, also copies the entity's orientation (Vec4f).
Returns nonzero on success, zero on failure (prints error message).
All trigger handlers pass NULL for out_rotation (position only).

### CopyRemainingArgs @ 0x004d6d00

Copies the remaining unparsed portion of the console command line to the given buffer.
Used by script-only triggers to capture the script filename as the last argument.

### FUN_0050c400 @ 0x0050c400
Called when TIME_LIMIT trigger expires and has a script_name. Likely executes the
end-of-round script.

### FUN_00510d80 @ 0x00510d80
Called when TIME_LIMIT trigger deadline is reached. Likely the "game over" handler.

### FUN_00548760 @ 0x00548760
Door toggle function. Called by DOOR/DOOR_ONCE/DOORS_EITHER trigger handlers
after resolving door actor names from the target list via GetTokenValue.

## Network Events

When triggers fire or reset, network broadcast messages are sent via FUN_00504bf0:
- `0x51` - Timed trigger activation (TIME, TIME_IF_ALIVE, TIME_LIMIT fire)
- `0x52` - Trigger reset (condition becomes false, rearms trigger)
- `0x3d` - Death event (actor died, checked by DEATH triggers)
- `0x8d` - One minute warning (TIME_LIMIT approaching deadline)
- `0xb2` - Actor lit up/spotted (LIGHT_UP trigger fires)

## Remaining Unknowns

1. **field_0x34** and **field_0x64**: Never accessed in trigger evaluation (EvaluateTriggers).
   Likely struct padding. Could verify by checking save/load game functions.

2. **FUN_0050be10**: References FirstTrigger 4 times. Might be a trigger serialization
   or cleanup function. Not yet analyzed.

3. **SaveGame?** @ 0x005083d6: References FirstTrigger. May contain trigger save/load
   logic that could reveal the purpose of padding fields.

4. **TriggerList/Trigger naming**: DONE for the node structs - the former `Trigger`
   (string node) is now `ActorNameNode`, and the real global trigger node (formerly
   `Trigger2`) now holds the clean name `Trigger`. `TriggerList` was left as-is: it is
   a generic string/node container but the name is used broadly; rename to
   `StringList`/`ActorNameList` remains optional future cleanup.

5. **QueueScriptExecution**: The actual prototype and mechanism of script execution
   needs further analysis. Called at LAB_0051066d when a trigger fires.
