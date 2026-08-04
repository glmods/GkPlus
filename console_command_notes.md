# Console commands: the full registry, and what a script can use instead

Every console command Gunlok registers, recovered from the binary, classified by whether the
same feature is reachable from JavaScript. This is the inventory that says which `gk` bindings
are missing and which do not need to exist.

## 1. Recovering the registry

`RegisterConsoleCommand` @ 0x004d5d50 has **280 call sites**, across four functions:

| Registrar | Address | Registrations |
|---|---|---|
| `SetupConsoleCommands` | 0x0043c800 | 258 |
| `ShowBriefingOrDebriefScreen` | 0x004b1f60 | 9 |
| `SetupBriefDebriefCommands?` | 0x004b2410 | 7 |
| `BeginLevelSession` | 0x004e2560 | 6 |

280 registrations, **272 distinct names**. Eight are registered twice — `DARK`, `BITMAP`,
`FADE IN`, `FADE OUT`, `BRIEFING TEXT`, `CLS` and `END BRIEFING` by both briefing registrars,
and `STOP TRACKING` twice in `SetupConsoleCommands` with two different help strings. Many more
are aliases: distinct names sharing one handler (`ADD PP` / `ADD PATROLPOINT`, `QUEUE SIZE` /
`QUEUE LENGTH`, the four spellings of `SET CAMERA POS`). 255 distinct handler addresses in all.

Two things make the extraction less mechanical than it looks:

- **Fifteen names are not in the executable.** `SetupConsoleCommands` passes
  `GetResourceString(&LocalizedStrings, id)` for both the name and the help of some commands, so
  the string comes from **`glres<lang>.dll`** — `glreseng.dll` for English, `glresfr`, `glresgr`,
  `glresit`, `glressp` otherwise. `WinMain` @ 0x0046b2e9 picks the DLL and
  `LoadResourceStringTable` @ 0x00578f30 pulls ids 0..0x7532 out of it with `LoadStringA`. The
  affected commands are `EXIT`, `QUIT`, `MENU`, `HELP`, `LINES`, `CONSOLE APPEAR`, `SAY`, `TIME`,
  `DATE`, `LIST COMMANDS`, `CLEAR HISTORY BUFFER`, `HISTORY BUFFER SIZE`,
  `HISTORY_BUFFER_LENGTH`, `QUEUE SIZE` and `QUEUE LENGTH`.

  **This is a live hazard for scripts**: `console.execute("QUIT")` does nothing on a French or
  German install. `console.commands` exists to make those spellings discoverable at runtime.
- One registration has an **empty name** (site 0x0043da6a) with a `RET` stub for a handler, which
  is what makes a blank console line a silent no-op. And one name carries a **trailing space**,
  `"BORDERS "`, which matters because matching is by prefix.

**Re-extracting it.** `RegisterConsoleCommand` is `__fastcall(name in ECX, help in EDX, callback,
condition)`, so a linear scan of each registrar tracking ECX/EDX/pushes recovers all four
arguments. The one wrinkle is telling a literal from a resource id: track whether the register was
last written by a `CALL GetResourceString` (@ 0x00579000, `this` = `&LocalizedStrings`
@ 0x00725664) — if so its value is the **id in EDX at that call**, otherwise it is a `.rdata`
pointer to read directly. Resolve the ids against `glreseng.dll`'s `RT_STRING` resources: block
`(id >> 4) + 1`, entry `id & 15`, each a `uint16` length in UTF-16 code units followed by that
many code units.

## 2. How a command is stored and dispatched

The registry is a **hash table**, not a list. `src/Console.h` modelled it as a doubly-linked list
anchored at 0x007b6aa8, which is `CommandsToExecute` — the unrelated per-frame command *queue*.
The real thing is at 0x007b6a70..0x007b6a7c:

| Offset | Name |
|---|---|
| 0x007b6a70 | `NumRegisteredCommands` |
| 0x007b6a74 | `CommandTableNumBuckets` |
| 0x007b6a78 | `CommandTableMask` |
| 0x007b6a7c | `CommandTableBuckets` (`CommandListElem **`) |

`CommandData` is `{const char *name; const char *help; callback; int condition;}` (0x10) and
`CommandListElem` is `{CommandData *command; CommandListElem *next;}` (8). The hash
(`HashCommandName` @ 0x004d4290) is just the uppercased first character of the name, masked;
registration prepends, and nothing is ever removed.

`ConsoleExecuteCommandLine` @ 0x004d59e0 walks the one bucket, keeps the **longest**
case-insensitive prefix match, strips that prefix from the line and calls the handler with the
remainder. That is why `SET CAMERA POSITION` and `SET` can coexist, and why a trailing space in a
registered name is meaningful.

**`condition` is a gate, and it is inert in a stock build.** Dispatch runs the handler only when
`command->condition <= CommandCondition` (@ 0x006a642c). Registrations use 0, 1 and 3; the global
is initialised to **3** and the only writer, `HandleConsoleKeyPress`, sets it to 3 and then
restores it. So every command passes, including from a `.gcs` and from `console.execute`. Worth
knowing because it *looks* like an interactive-only gate on half the table, and it is not.

## 3. Classification

The question each command was put to is: **can a script get this effect through the `"gk"`
module or through plain JavaScript?** `console.execute` is excluded on purpose — it reaches
everything by construction, so counting it would make the answer trivially yes and useless.

Totals across the 272 distinct names:

| | Count |
|---|---|
| Plain JavaScript replaces it outright | 7 |
| Already reachable through a typed `gk` binding | 33 |
| Newly bound | 223 |
| Developer scaffolding — nothing to bind | 9 |
| Still a gap | **0** |

**Every command is covered.** The 25 that broadcast were the last holdouts, and it turned out they
needed no wire-format work at all - see §4. §4.1 still lists them with their recovered update ids,
because knowing *which* commands replicate is what tells a script author whether a call is
authority-gated.

The tables below were generated from the extracted registry with a completeness check — every
one of the 272 registered names appears in exactly one of them, and an unclassified name is an
error rather than a silent omission. To rebuild after a binding lands, re-extract per §1 and
re-classify; do not hand-edit a row without re-running the check.

### Plain JavaScript replaces it outright (7)

| Command | Replacement |
|---|---|
| `//` | `//` |
| `DATE` | `new Date()` |
| `DEC` | `tokens[n]--` |
| `IF` | `if` |
| `INC` | `tokens[n]++` |
| `REM` | `//` |
| `TIME` | `new Date()` |

### Already reachable through a typed `gk` binding (33)

| Command | Replacement |
|---|---|
| `ADD` | `roles[name].spawn(team, pos)` / `level.spawn` |
| `ADD TEAM` | `roles[name].spawn(team, pos)` |
| `ADD TRIGGER` | `triggers.create({...})` |
| `ADDTEAM` | `roles[name].spawn(team, pos)` |
| `ASSOCIATE` | `actor.associate(script, true)` |
| `BATCH` | `console.execute_file(path)` |
| `BATCHANDBROADCAST` | `level.send("file.gcs")` |
| `CURSOR COLOUR` | `console.cursor_color` |
| `DELETE` | `actor.remove()` |
| `EXCEPTCTF` | `if (game.mode !== "capture_the_flag")` |
| `EXCEPTPRESIDENT` | `if (game.mode !== "president")` |
| `FRAG` | `actor.frag()` |
| `GET ACTOR ARMOUR` | `actor.armor` |
| `GET ACTOR NAME` | `actor.name` |
| `GOTO` | `actor.goto(dest)` |
| `HURT GUNLOK IF NECESSARY` | `actor.damage(n)` |
| `LINK` | `actor.associate(script, false)` |
| `LIST TOKENS` | `Object.entries(tokens)` |
| `LIST UNITS` | `for (const a of actors)` |
| `ONLYCTF` | `if (game.mode === "capture_the_flag")` |
| `ONLYPRESIDENT` | `if (game.mode === "president")` |
| `REQUIRES` | `pickup.set_required_item(name)` |
| `SET` | `tokens[name] = value` |
| `SET ACTOR ARMOUR` | `actor.armor = n` |
| `SET CAMERA DISTANCE` | `camera.distance` |
| `SET DISTANCE` | `camera.distance` |
| `SET MAX DISTANCE` | `camera.max_distance` (now writes both globals) |
| `SHOW ZOOM` | `camera.distance` |
| `TELEPORT` | `actor.set_position(pos)` — **single player only**, see §4.1 |
| `TELEPORT AND ORIENTATE` | `actor.set_position(pos, ori)` — **single player only**, see §4.1 |
| `TEXT` | `console.print(text)` |
| `TEXT COLOUR` | `console.text_color` |
| `TOKEN` | `tokens[name] = value` (an upsert) |

### Newly bound in this pass (223)

| Command | Replacement |
|---|---|
| `@` | `console.echo` |
| `ADD BLINKING LIGHT` | `light.add_blinking(...)` |
| `ADD LIGHT` | `light.add(where, r, g, b)` |
| `ADD MISSION` | `screen.add_mission(script, console?)` |
| `ADD MULTI MISSION` | `screen.add_multiplayer_mission(...)` |
| `ADD MULTIMISSION` | `screen.add_multiplayer_mission(...)` |
| `ADD OBJECTIVE` | `objectives.add(...)` |
| `ADD PATROLPOINT` | `units.add_patrol_point(where?)` |
| `ADD PP` | `units.add_patrol_point(where?)` |
| `ADD WAYPOINT` | `units.add_waypoint(where?)` |
| `ADDMISSION` | `screen.add_mission(script, console?)` |
| `ADDMULTIMISSION` | `screen.add_multiplayer_mission(...)` |
| `AI` | `units.set_ai(actor, ai)` |
| `AIRSTRIKE` | `fx.airstrike(a, b)` |
| `ALERT NODE` | `units.alert_node(node)` |
| `ALTER CAMERA` | `camera.nudge(dx, dy)` |
| `AMBIENT` | `world.set_ambient({r, g, b, a})` |
| `ANIM` | `units.play_animation(actor, n)` |
| `ASSOCIATELIGHT` | `light.associate(...)` |
| `ATTACH` | `tracks.attach(actor)` |
| `BASIN` | `fx.basin(...)` |
| `BATTLE NUMBER` | `game.battle_number` |
| `BITMAP` | `screen.bitmap(name)` |
| `BOARD` | `units.board(target)` |
| `BORDERS ` | `screen.borders(width, speed)` |
| `BORDERS OFF` | `screen.borders_off()` |
| `BRIEFING TEXT` | `screen.briefing_text(n)` |
| `CAMERA TRACK` | `camera.bezier_track(p1, p2, p3, p4)` |
| `CANCEL WAIT FOR` | `script.cancel_wait_for()` |
| `CD AUTO` | `music.cd_auto(on)` |
| `CD FADE` | `music.cd_fade(vol, secs)` |
| `CD PLAY` | `music.cd_play(loop, tracks)` |
| `CD SET VOLUME` | `music.cd_set_volume(v)` |
| `CD STOP` | `music.cd_stop()` |
| `CD TRACKS` | `music.cd_tracks(category, tracks)` |
| `CD VICTORY KILLS` | `music.victory_kills(n)` |
| `CENTRE` | `camera.center_on(id)` |
| `CHECK WAIT FOR` | `script.check_wait_for()` |
| `CHROME` | `game.chrome` |
| `CLEAR HISTORY BUFFER` | `console.clear_history()` (localized name) |
| `CLOSE DOOR` | `tracks.close_door(id)` |
| `CLS` | `screen.clear()` |
| `COMPLETE OBJECTIVE` | `objectives.complete(n)` |
| `CONSOLE APPEAR` | `console.set_appear(instant)` (localized name) |
| `CONTROLS` | `game.controls_enabled` |
| `CORONA` | `light.corona(where?, color?)` |
| `CREDITS` | `screen.credits()` |
| `CURSOR` | `screen.cursor(on)` |
| `DARK` | `light.dark()` |
| `DEACTIVATE ELECTRICITY` | `fx.deactivate_electricity(endpoint)` |
| `DEFOGGER` | `units.make_defogger(actor)` |
| `DELETE TEAM` | `units.delete_team(n)` |
| `DETACH` | `tracks.detach(actor)` |
| `DOOR` | `tracks.declare_door(where, id)` |
| `EASY` | `if (game.difficulty === "easy")` |
| `ECHO` | `console.echo` |
| `ELECTRICITY` | `fx.electricity(a, b, amplitude)` |
| `ELEVATE CAMERA` | `camera.elevate(units, secs)` |
| `END BRIEFING` | `screen.end_briefing()` |
| `EPW` | `game.epw` |
| `EWS` | `fx.explode_with_smoke(where?)` |
| `EXIT` | `screen.quit()` (same handler; localized name) |
| `EXPLODE` | `fx.explode(where?)` |
| `EXTREME` | `if (game.difficulty === "extreme")` |
| `FADE FROM BLACK` | `light.fade_from_black(s)` |
| `FADE IN` | `light.fade_in()` |
| `FADE OUT` | `light.fade_out()` |
| `FADE TO BLACK` | `light.fade_to_black(s, hold)` |
| `FAIL OBJECTIVE` | `objectives.fail(n)` |
| `FLARE FIRER` | `units.make_flare_firer(actor)` |
| `FMV` | `screen.end_game_fmv(which)` |
| `FOG` | `world.fog.enabled` |
| `FOGCOLOUR` | `world.fog.color` |
| `FOGGER` | `units.clear_defogger(actor)` |
| `FOGTRANSITION` | `world.fog.transition` |
| `FOGUPDATE` | `world.fog.update_rate` |
| `FOGVALUE` | `world.fog.value` |
| `FREE CAMERA FOCUS` | `camera.focus = null` |
| `FRIENDLY FIRE` | `game.friendly_fire` |
| `GAMESPEED` | `screen.game_speed(n)` |
| `GET ACTOR ID` | `game.actor_under_cursor` |
| `GET CAMERA` | `camera.position` + `camera.orientation` |
| `GET SELECTED` | `game.selected_actor` |
| `GET WAIT FOR` | `script.print_wait_for()` |
| `GIVE` | `inventory.give(who, item)` |
| `GIVE AND EQUIP` | `inventory.give_and_equip(...)` |
| `GIVE AND EQUIP AND SAY` | `inventory.give_and_equip_and_say(...)` |
| `GIVE AND EQUIP ROLE` | `inventory.give_and_equip_role(...)` |
| `GIVE AND EQUIP ROLE ID` | `inventory.give_and_equip_role_id(item)` |
| `GIVE AND EQUIP ROLE TEAM` | `inventory.give_and_equip_role_team(...)` |
| `GIVE AND SAY` | `inventory.give_and_say(...)` |
| `GIVE CONTROL` | `units.give_control(actor, team)` |
| `GIVE ROLE` | `inventory.give_role(...)` |
| `GIVE ROLE ID` | `inventory.give_role_id(item)` |
| `GIVE ROLE TEAM` | `inventory.give_role_team(...)` |
| `HARD` | `if (game.difficulty === "hard")` |
| `HEAP` | `inventory.heap(actor, ...items)` |
| `HELP` | `console.commands.find(c => c.name === ...).help` |
| `HIDE CONSOLE` | `console.hide()` |
| `HISTORY BUFFER SIZE` | `console.history_size(n)` (localized name) |
| `HISTORY_BUFFER_LENGTH` | `console.history_size(n)` (localized name) |
| `HUNTER` | `units.make_hunter(actor)` |
| `IF CARRYING` | `inventory.if_carrying(item, command)` |
| `LASER FENCE` | `fx.laser_fence(a, b)` |
| `LAVA` | `fx.lava(...)` |
| `LIGHT CYLINDER` | `light.cylinder(where)` |
| `LIGHTNING` | `fx.lightning(on)` |
| `LIGHTON` | `light.light_on(...)` |
| `LINES` | `console.set_lines(n)` (localized name) |
| `LIST COMMANDS` | `console.commands` |
| `LIST TEAM` | `units.list_team(n)` |
| `LOAD` | `demo.load(name)` |
| `LOG` | `console.write_log(note)` |
| `LOWER LAVA` | `fx.lower_lava(where, n)` |
| `LOWER WATER` | `fx.lower_water(where, n)` |
| `MEDIUM` | `if (game.difficulty === "medium")` |
| `MENU` | `screen.main_menu()` (localized name, resolved at call) |
| `NEW NODE WAYPOINT LIST` | `units.new_node_waypoint_list(node)` |
| `NEXT LEVEL` | `screen.next_level()` |
| `NEXT RESPAWN ID` | `inventory.next_respawn_id()` |
| `OBJECTIVE TEXT` | `objectives.print(n)` |
| `OIL` | `fx.oil(...)` |
| `OPEN DOOR` | `tracks.open_door(id)` |
| `PARTICLE TESTER` | `fx.particle_tester(type, where?)` |
| `PAUSE` | `tracks.pause(name)` |
| `PAUSE GAME` | `screen.toggle_pause()` (`game.paused` reads it) |
| `PLAY CUTSCENE` | `screen.play_cutscene(name)` |
| `PLAY ENVIRONMENTAL SOUND` | `music.play_environmental_sound(id)` — broken in the game |
| `PLAY FMV` | `screen.play_fmv(name)` |
| `PLAY SOUND` | `music.play_sound(id)` |
| `PLAYBACK` | `demo.playback()` |
| `PLAYER SELECT` | `units.player_select(actor)` |
| `PRESIDENT` | `units.create_president()` |
| `PULSE RINGS` | `fx.pulse_rings(...)` |
| `QUEUE LENGTH` | `console.queue_size(n)` (localized name) |
| `QUEUE SIZE` | `console.queue_size(n)` (localized name) |
| `QUIT` | `screen.quit()` (localized name, resolved at call) |
| `RAIN` | `fx.rain(on)` |
| `RAISE LAVA` | `fx.raise_lava(where, n)` |
| `RAISE WATER` | `fx.raise_water(where, n)` |
| `RATE` | `fx.particle_rate(n)` |
| `RAY` | `light.ray(where?)` |
| `RAY COLOUR` | `light.ray_color(r, g, b)` |
| `REAL WAIT` | `script.real_wait(s)` |
| `REAL WAIT OR CLICK` | `script.real_wait_or_click(s)` |
| `REB GOD` | `game.god_mode` |
| `REB INFINITE AMMO` | `game.infinite_ammo` |
| `RECORD` | `demo.record()` |
| `REFLECT` | `light.reflect()` |
| `REMOVE ITEM` | `inventory.remove_item(role)` |
| `REMOVE LIGHT CYLINDER` | `light.remove_cylinders()` |
| `REMOVE PULSE RINGS` | `fx.remove_pulse_rings(endpoint)` |
| `REMOVE TRIGGER` | `units.remove_trigger(...)` |
| `REMOVEBB` | `units.remove_bounding_box(actor)` |
| `REPTXT` | `objectives.repeat_text(n)` |
| `RESPAWN HEAP` | `inventory.respawn_heap(...)` |
| `RESTORE CAMERA` | assign that variable back |
| `RING` | `fx.ring(...)` |
| `ROTATE CAMERA` | `camera.rotate(units, secs)` |
| `RUN TRACK` | `tracks.run(name)` |
| `SAVE` | `demo.save(name)` |
| `SAVE CAMERA` | a plain variable: `{...camera.position}` + `camera.orientation` |
| `SAY` | `screen.say(message)` (localized name; sends via a native) |
| `SEA` | `fx.sea(...)` |
| `SELECT` | `game.selected_actor = id` |
| `SET ACTIVITY` | `units.set_activity(actor, activity)` |
| `SET CAMERA` | `camera.position` |
| `SET CAMERA FOCUS` | `camera.focus = pos` |
| `SET CAMERA ORI` | `camera.orientation` |
| `SET CAMERA ORIENTATION` | `camera.orientation` |
| `SET CAMERA POS` | `camera.position` |
| `SET CAMERA POSITION` | `camera.position` |
| `SET JERKY DISTANCE` | `camera.jerky_zoom_to(d)` |
| `SET LOOP TIME` | `tracks.set_loop_time(s)` |
| `SET LOWER RIGHT BOUND` | `units.set_lower_right_bound(where)` |
| `SET REQUIRED DISTANCE` | `camera.zoom_to(d, secs)` |
| `SET REQUIRED ORI` | `camera.turn_to(y, r, p, secs)` |
| `SET REQUIRED POS` | `camera.move_to(where, secs)` |
| `SET REQUIRED POSDIST` | `camera.move_and_zoom_to(...)` |
| `SET SCALE` | `units.set_scale(scale, actor?)` |
| `SET SPEED` | `tracks.set_speed(scale, ...names)` |
| `SET TRACK` | `tracks.set(...)` |
| `SET TRAINING AREA` | `game.training_area` |
| `SET UPPER LEFT BOUND` | `units.set_upper_left_bound(where)` |
| `SET WATER DIRECTION` | `fx.set_water_direction(where, dir)` |
| `SET WATER SPEED` | `fx.set_water_speed(where, n)` |
| `SHADOW` | `light.shadow(actor)` |
| `SMOKE` | `fx.smoke(actor)` |
| `SNOW` | `fx.snow(on)` |
| `SPARKS` | `fx.sparks(where?)` |
| `SPAWN` | `game.spawn(n)` with `game.difficulty` for the scaling |
| `SPAWN TEAM` | `game.spawn(n, team)` |
| `SPAWNTEAM` | `game.spawn(n, team)` |
| `SPEAK` | `units.speak(unit, message)` |
| `SPOTLIGHT` | `light.spotlight(where?)` |
| `START PRINTING OBJECTIVES` | `objectives.start_printing(delay?)` |
| `STATS SCREEN` | `screen.stats()` |
| `STATUS WINDOW` | `screen.status_window(mode)` |
| `STOP PARTICLES` | `fx.stop_particles(actor)` |
| `STOP TRACKING` | `camera.stop_tracking()` |
| `SUNANGLE` | `world.sun_angle` |
| `SUNANGLE2` | `world.sun_angle2` |
| `SUNBRIGHTNESS` | `world.set_sun_brightness({r, g, b, a})` |
| `SWAMP` | `fx.swamp(...)` |
| `SWITCH DETAIL LEVEL` | `game.low_detail` |
| `SYSTEM CURSOR` | `screen.system_cursor()` |
| `TEXTURE ANIMATE` | `fx.texture_animate(...)` |
| `TOGGLE DOOR` | `tracks.toggle_door(id)` |
| `TRACK` | `camera.track(actor_id)` |
| `TRAINING DEBRIEF TEXT` | `screen.training_debrief_text(n)` |
| `TRNTXT` | `objectives.training_text(n)` |
| `TURN HEARING RANGE` | `units.turn_hearing_range(on, actor)` |
| `TURN VISION CONE` | `units.turn_vision_cone(on, actor)` |
| `TURRET LOS` | `units.turret_los(on, turret)` |
| `UNPAUSE` | `tracks.unpause(name)` |
| `VERSION` | `console.print_version()` |
| `VISION` | `game.vision_cones` |
| `VULNERABILITY` | `units.set_vulnerability(...)` |
| `WAIT` | `script.wait(s)` |
| `WAIT FOR` | `script.wait_for(cond)` |
| `WATCH` | `units.watch(actor)` |
| `WATER` | `fx.water(...)` |
| `WIREFRAME` | `game.wireframe` |

### Developer scaffolding - nothing to bind (9)

| Command | Replacement |
|---|---|
| `<empty>` | the RET stub that makes a blank console line a no-op |
| `ASSERT` | throws a breakpoint |
| `BREAK` | an empty function the developers kept a breakpoint in |
| `CENTRE CURSOR TEST` | "Just for testing GlCentreCursor()" |
| `CLEAR DIALOGS` | "I neither need nor give help" |
| `CONFIRM DIALOG TEST` | "I neither need nor give help" |
| `INFO DIALOG TEST` | "I neither need nor give help" |
| `RESET TIME` | help text is literally "Alex - TEST" |
| `TIMER INFO` | help text is literally "Alex - TEST" |

### Still a gap: reachable only through `console.execute` (0)

**Demo record and playback** (0) — 

## 4. How the bindings divide, and why the broadcasters were not special

There are two kinds of binding here, and the split is not by subsystem.

- **Native** - `camera`, `game`, `world`, and the `console` colours and registry. Used wherever there
  is **state to read back**: an accessor has to reach the global, and no console command returns a
  value to a script. These are also the ones where a script needs precision, like the camera's
  Euler angles.
- **Command-backed** - `fx`, `light`, `objectives`, `music`, `screen`, `units`, `inventory`,
  `tracks`, `demo`, `script`, the interpolated `camera` moves and the `console` administration.
  These format a console command line and run the game's own handler.

The second choice is deliberate, and it is *more* faithful than the alternative. Every one of those
handlers **is** an argument parser: `CommandWater` is eight `ConsoleParse*` calls whose defaults come
from `TheMap->bounds_min`/`bounds_max`, `CommandExplode` falls back to the cursor, `CommandGive`
resolves a role and walks an inventory. Reimplementing ~200 of those in C++ is ~200 chances to
diverge on a default, a clamp or an omitted-argument rule - silently. Dispatching the real handler
keeps the game's parsing, defaults, range checks *and* its executor handshake, for free.

**And that is why the broadcasters cost nothing extra.** An update id and payload layout only have to
be reproduced by a *native* binding. Dispatching runs the handler, which does its own
`IsExecutorRunning` check, its own `SuspendExecutor` bracket and its own `BroadcastToPlayers`. The
"wire-format work" that made those 25 look hard was an artefact of the approach, not a property of
the commands.

They are also **safe to call unguarded**, which is the useful part for a script author: 24 of the 25
gate on `IsExecutorRunning`, so on a joining client they are a silent no-op while the authority
machine mutates and broadcasts. That is the opposite of the trap in §6 - `actor.set_position` has no
such gate, so it happily moves a local ghost. (`STATS SCREEN` is the exception that needs no gate:
it sends *to* the server, which is the right direction from a client.)

What the binding adds over a bare `console.execute` string is real, and it is the whole reason these
are not just documentation:

- **Typed, named arguments**, checked by `types/gk.d.ts`.
- **Locale-independent numbers.** `snprintf` follows this DLL's locale; a decimal comma would reach
  an `atof` that expects a point. The formatter rewrites it.
- **Names that survive a translated install.** `screen.quit()`, `screen.main_menu()`, `screen.say()`
  and the six `console` administration members resolve their name from `glres<lang>.dll` at the
  call, because those names are not in the executable at all (§1).
- **A length check the engine does not do.** `ExecuteCommand` copies into `ConsoleCommandLine`
  (`char[252]`) with an *unbounded* byte loop, and the next global is `ConsoleSmallFont`. Nothing in
  the game can reach that - `fgets` caps a batch line at 249 - but a script can. Both the dispatcher
  and `console.execute` now refuse an over-long line rather than truncating it.
- **Whitespace handling that matches the handler.** The console splits arguments on whitespace and
  has no quoting, so a string containing a space is refused - except for the handlers that take the
  rest of the line (`LOG`, `SAY`, `SPEAK`, `WAIT FOR`, `PLAY FMV`, `PLAY CUTSCENE`, `BITMAP`,
  `IF CARRYING`), where it is passed through. That distinction is per command, read off the
  handler's own `CopyRemainingArgs` vs `ConsoleParseWord`.

Two things stayed out of the surface, with reasons:

- **`PLAY ENVIRONMENTAL SOUND` is broken in the game** - the handler parses a sound id, tests it
  non-zero, then calls the player with a hard-coded argument and discards the id. It is bound so the
  surface is complete, with the defect recorded on the declaration.
- **The console-queue pacing commands** (`script.wait` and friends) suspend the *console* queue, not
  a script; they only affect work queued with `console.execute_file`. There is still no way to
  schedule a JavaScript callback for later except a `time` trigger - see §6.1 for why that is not
  merely an omission.

### 4.1 Which commands actually broadcast, and which get it for free

Measured, not inferred: of the 252 handlers with a defined function, **27 call
`BroadcastToPlayers` @ 0x00504bf0 in their own body**, over 29 call sites. One calls
`SendToServer` (`STATS SCREEN`) and one `QueueScriptExecution` (`BATCHANDBROADCAST`).

The distinction that matters for binding is **who owns the broadcast**:

- If the handler broadcasts **itself**, a binding has to reproduce the update id and payload. These
  are the 27 below.
- If it delegates to a native that broadcasts **internally**, binding that native is enough. This
  is why `roles[].spawn` already replicates: `SpawnRole` @ 0x00503710 calls `BroadcastToPlayers`
  directly, so `ADD` and `ADD TEAM` came for free.

The 27, with the update id recovered from the dword written to the message buffer that reaches ECX
(the `msg` argument), and the payload size from EDX:

| Command | Handler | Update id [bytes] |
|---|---|---|
| `ADD LIGHT` | `CommandAddLight` | 0xc0 [40] |
| `ADD BLINKING LIGHT` | `CommandAddBlinkingLight` | 0xc1 [68] |
| `AIRSTRIKE` | `CommandAirstrike` | 0xc2 [28] |
| `ANIM` | `CommandAnim` | 0xba [12] |
| `BOARD` | `CommandBoard` | 0xb5 [8] |
| `CAMERA TRACK` | `CommandCameraTrack` | 0xaa |
| `CLOSE DOOR` | `CommandCloseDoor` | 0xbd [8] |
| `DEFOGGER` | `CommandDefogger` | 0xb7 [8] |
| `FOGGER` | `CommandFogger` | 0xb8 [8] |
| `GIVE CONTROL` | `CommandGiveControl` | 0x50 [12] |
| `OPEN DOOR` | `CommandOpenDoor` | 0xbc [8] |
| `PLAYER SELECT` | `CommandPlayerSelect` | 0xc3 [8] |
| `REMOVE ITEM` | `CommandRemoveItem` | 0x7d [12] |
| `REMOVEBB` | `CommandRemoveBB` | 0xbb [8] |
| `SET ACTOR ARMOUR` | `CommandSetActorArmor` | 0x93 [12] |
| `SET LOOP TIME` | `CommandSetLoopTime` | 0xac [12] |
| `SET SPEED` | `CommandSetSpeed` | 0xab [12] |
| `SET TRACK` | `CommandSetTrack` | 0xa9 |
| `SHADOW` | `CommandShadow` | 0xc4 [8] |
| `SMOKE` | `FUN_00448640` | 0xbe [8] |
| `SPEAK` | `CommandSpeak` | *computed* |
| `STOP PARTICLES` | `CommandStopParticles` | 0xbf [8] |
| `TELEPORT` | `CommandTeleport` | 0x3d [56] **and** 0x6f [40] |
| `TELEPORT AND ORIENTATE` | `CommandTeleportAndOrientate` | 0x70 [40] **and** 0x3d [56] |
| `TEXTURE ANIMATE` | `CommandTextureAnimate` | *computed* |
| `TOGGLE DOOR` | `CommandToggleDoor` | *computed* (0xbc or 0xbd, per the open/closed test) |
| `TRACK` | `CommandTrack` | 0xb4 [8] |

Three sites build the id in a register rather than storing a literal, so they need disassembly
rather than a scan — `TOGGLE DOOR` picks 0xbc/0xbd from the door's current state, and `SPEAK` and
`TEXTURE ANIMATE` both pass a register-held buffer with a variable-length payload.

**`actor.set_position` is local-only, and `TELEPORT` is not.** `Actor::SetPositionAndOrientation`
@ 0x0052ded0 and `MobileActor`'s override @ 0x00539ae0 reach no broadcast (checked to call depth
4), while `CommandTeleport` broadcasts **two** updates around its own call to the same setter. So
`actor.set_position` moves the actor on the calling machine and nowhere else: it is equivalent to
`TELEPORT` in single player, and silently desynchronising in multiplayer. Anything that must move
an actor for every player wants `console.execute("TELEPORT …")` until 0x3d/0x6f are bound.

Measured across the 178 gaps: **125 are purely local**, **26 need only the
`SuspendExecutor`/`ResumeExecutor` handshake** (which is two calls, not wire format), and **25
carry a broadcast** — the 27 above minus `TELEPORT` and `TELEPORT AND ORIENTATE`, which are
counted as already-bound-with-a-caveat. So the wire-format problem covers about one gap in seven;
the rest is effort, not risk. The largest broadcast-free clusters still open are the world effects
(26 local), the interpolated camera moves (9), mission objectives (7), CD music and sound (9), and
the session/presentation toggles (21).

Three clusters are gaps for their own reasons rather than that one:

- **Console-queue pacing** (`WAIT`, `WAIT FOR`, and friends) suspends the *console command queue*,
  which a script does not use. The JS equivalent would be a scheduler — a per-frame or timer
  callback — and the host has the seam already (`SetFrameCallback` in `src/GUI.h`, which
  `BootScriptHost` uses to drain the job queue) but exposes nothing to scripts. Today a script's
  only way to run code later is a `time` trigger. This is the most valuable single gap left.
- **`LIST TEAM`** is unreachable for a smaller reason: `Actor` has no `team` getter. `set_team`
  exists, and `Actor::team_id` sits at +0xbc, but the wrapper's surface is deliberately the vtable
  rather than the fields, so reading it needs either a vtable getter or a documented exception the
  way `id` and `name` already are.
- **`REMOVE TRIGGER`** has a native (`RemoveTrigger` @ 0x0050c400) and no binding, so `triggers`
  can create but not destroy.

`console.execute` remains the escape hatch for all 178 — with the caveat from §1 that fifteen
names are localized, so anything reaching for `EXIT`, `QUIT`, `MENU`, `SAY`, `HELP`, `TIME`,
`DATE`, `LINES`, `CONSOLE APPEAR`, `LIST COMMANDS` or the history/queue-size pairs should look
the name up in `console.commands` rather than spell it.

## 6. What replicates, and what only looks like it does

§4.1 asks which *commands* broadcast. The mirror question matters more for scripts: which of the
**engine setters the bindings call** broadcast. They are not the same set, and where they differ the
console command is the one that broadcasts — *around* a setter that does not.

Measured by forward reachability to `BroadcastToPlayers` @ 0x00504bf0 or `SendToServer`
@ 0x004fdbc0, to call depth 4:

| Binding | Engine setter | Replicates |
|---|---|---|
| `actor.health` | `Actor::SetHealth` @ 0x0052dbc0 | **yes** |
| `actor.damage()` | `MobileActor::ApplyDamage` @ 0x00535ac0 | **yes** |
| `actor.damage()` on a non-mobile | `Actor::ApplyDamage` @ 0x0052f3b0 | no, but see below |
| `actor.frag()` | `Actor::Frag` @ 0x0052e220 | **yes** |
| `actor.remove()` | `Actor::Delete` @ 0x0052f0d0 | **yes** |
| `actor.die()` | `MobileActor::Die` @ 0x0053a020 | **yes** |
| `actor.set_target()` / `clear_target()` | `Actor::SetTarget` / `ClearTarget` | **yes** |
| `actor.associate()` | `PickupActor::Associate` @ 0x005469f0 | **yes** |
| `actor.dissociate()` | `MobileActor::Dissociate` @ 0x00535f60 | **yes** |
| `actor.set_weapon()` / `set_ammo_type()` | `CharacterActor::…` | **yes** |
| `actor.attack_target()` / `attack_position()` / `stop_attacking()` | `CharacterActor::…` | **yes** |
| `pickup.set_pickup_enabled()` | `PickupActor::SetPickupEnabled` @ 0x00546240 | **yes** |
| `role.spawn()` / `level.spawn()` | `SpawnRole` @ 0x00503710 | **yes** |
| `actor.armor` | `Actor::SetArmorValue` @ 0x0054f360 | **no** |
| `actor.shield` | `Actor::SetShieldValue` @ 0x0054f440 | **no** |
| `actor.set_position()` | `Actor::SetPositionAndOrientation` @ 0x0052ded0 | **no** |
| `actor.set_team()` | was `SetTeamId`, now `ChangeOwnerAndTeam` @ 0x00530470 | **yes**, since d11995b |
| `actor.concealed` (was `actor.mine`) | `Actor::SetConcealed` @ 0x0054e890 | **no** |
| `actor.goto()` | `MobileActor::Goto` @ 0x00539450 | **no** |
| `turret.turret_enabled` | `TurretActor::SetTurretEnabled` @ 0x0054e8b0 | **no** |
| `pickup.set_required_item()` | `PickupActor::SetRequiredItem` @ 0x00546b20 | **no** |
| `tokens[name] = value` | `SetOrCreateToken` @ 0x004d35f0 | **no** |
| `triggers.create()` | `RegisterTriggers` @ 0x0043e240 | **no**, by design |

Three of these deserve more than a row:

- **`set_team` was calling the wrong one of a matched pair, and it is now fixed.**
  `Actor::SetTeamId` @ 0x0054e680 is one instruction, `this->team_id = team`. Every engine site that
  changes a team brackets it with a removal from the old team's actor list and an insert into the
  new one, gated on `+0x3c` — so calling it bare also left the actor **on its old team's list**,
  which is a correctness bug before replication even enters into it.
  `Actor::ChangeOwnerAndTeam` @ 0x00530470 does the list move and broadcasts update 0x58 (with the
  `+0x28`/`+0x2c` fields) followed by 0x50 (the team). The binding now calls it, passing the actor's
  current `+0x28`/`+0x2c` back in so it stays a team change.

  Worth recording that **`CommandGiveControl` does not call it** — it reproduces the 0x50 half
  inline. So "the command that broadcasts" and "the setter that broadcasts" are two different pieces
  of code that happen to agree, which is why the first pass mis-described the relationship.
- **`triggers.create` being local is correct**, not a defect. In a stock level every machine runs
  the same `.gcs` and registers its own copy, and the *payload* is what replicates when the trigger
  fires. The practical rule is the inverse of the usual one: register triggers in `setup`, which
  runs on every machine, and do **not** guard that with `game.simulation_running` — a guard would
  leave joining clients without the trigger.
- **Tokens are local but saved.** Writes do not replicate, so they have to be made from something
  every machine runs; but `SaveGame` writes them and the loader rebuilds them through
  `SetOrCreateToken`, which makes them the right home for script progress that must survive a save.

### 6.2 `ApplyDamage` is two different functions, and our mirror had its arity wrong

`Actor::ApplyDamage` @ 0x0052f3b0 and `MobileActor::ApplyDamage` @ 0x00535ac0 are not a base and a
tweak. The base is: subtract armour as a flat threshold, decrement `strength`, call `Frag` at zero.
The override adds god mode, a **shield** pool drained before armour, healing clamped to the
character's `strength`, Deathmatch frag credit, and a broadcast of update 0x9b.

So the base is not a version that "forgot" to replicate — it is the rule for actors that have no
shield and no networked health (pickups, blockers, projectiles, tumbleweeds). Their damage has no
observable state to sync, and the outcome that *is* observable — the `Frag` at zero strength —
broadcasts on its own. Nothing is missing.

**Both end in `RET 0xc`: three stack arguments, not two.** `src/Actors.h` declared
`ApplyDamage(float, bool)`, so every call pushed 8 bytes and the callee popped 12. `__thiscall` is
callee-clean, so that is 4 bytes of ESP drift per call — the failure mode CLAUDE.md describes at
length, surfacing as a non-deterministic access violation with EIP on the stack and a faulting
module of "unknown", nowhere near the call. Dormant until something calls it in a loop, which an
area-damage script over `actors` would do immediately.

The third argument is the **attacker's team id**, or -1 for none: `MobileActor::ApplyDamage` credits
the frag against it in Deathmatch when it differs from the victim's team, and sends it as the 0x9b
payload. `actor.damage(amount, use_armor, attacker_team)` now exposes it, defaulting to -1.

### 6.3 The vtable arity sweep

`ApplyDamage` turned out not to be alone. Every slot declared in `src/Actors.h` was checked against
the `RET` operand of the function the game actually puts in it, across all sixteen vtables — 1,460
slot entries. **Nine declarations were wrong.**

The check is exact, which is what makes it worth doing. `__thiscall` is callee-clean: `this` rides
in ECX and everything else is on the stack, so `RET n` states precisely how many bytes of arguments
the callee pops. A declaration that disagrees drifts ESP by the difference on every call.

| Slot | Declared | Actual | Corrected to |
|---|---|---|---|
| 20 `ApplyArmorDamage` | `()` | RET 0x4 | `(int)` |
| 21 `ApplyShieldDamage` | `()` | RET 0x4 | `(int)` |
| 27 `Stub27` | `()` | RET 0x4 | `(int)` |
| 55 `OnPrePhysics` | `()` | RET 0xc | `(int, int, int)` |
| 56 `OnCollisionResponse` | `()` | RET 0x8 | `(int, int)` |
| 59 `OnDamageReceived` | `()` | RET 0x8 | `(int, int)` |
| 65 `Delete` | `()` | RET 0x4 | `(bool broadcast)` |
| 68 `ApplyDamage` | `(float, bool)` | RET 0xc | `(float, bool, int attacker_team)` |
| 76 `OnAnimationComplete` | `()` | RET 0xc | `(int, int, int)` |
| 86 `QueueOrderPosition` | `(Vec3 *, int, char)` | RET 0x10 | adds a fourth `int` |

Only two were reachable from a script, and both were being called: `ApplyDamage` via `actor.damage()`
and `Delete` via `actor.remove()`. The other seven are landmines rather than live bugs — nothing in
GkPlus calls them yet.

**`Delete` was the worse of the two.** Its argument gates the 0x49 broadcast, and every game call
site passes 1. Declaring it `()` meant `actor.remove()` pushed nothing, so the callee read whatever
was on the stack: the removal replicated or not depending on garbage, *and* ESP drifted 4 bytes. It
now passes `true`.

Two false-positive sources are worth recording for anyone repeating this:

- **Slot 0 is exempt.** MSVC's scalar deleting destructor takes a hidden `int flags`, so it is
  always `RET 0x4` while `virtual ~X()` declares nothing. Not a mismatch.
- **A by-value `Vec3` is 12 bytes, not 4.** `SetExitPosition(Vec3)` and `SetTargetPosition(Vec3)`
  both look wrong under a naive one-dword-per-parameter model and are in fact correct.

Where an argument's purpose is unknown the declaration now uses `int` with a comment. The arity is
measured; the type is not, and one dword is what the slot pops either way.

### 6.4 The same sweep on Role, Map and the GLS sections

Running the §6.3 check over the other struct mirrors found **no further arity bugs**. Recording the
result because "we checked and it was clean" is worth as much as a finding:

- **The GLS section tables are correct.** `src/GLS.h`'s `ParsedThingVtbl` models 8 slots, and all
  **fourteen** section vtables (`ParsedShape` @ 0x006630b0 through `ParsedMap` @ 0x00663298) match it
  slot-for-slot by both function name and `RET` operand. This is the one mirror GkPlus actually calls
  through - `check_value`, `copy_fields`, `to_game_object` - so a mismatch would have been live.
- **One Ghidra label was wrong, not the mirror.** The table at 0x0066308c was labelled
  `ParsedThingBaseVtbl` and appears to break the model. It is a **nine**-slot table with an extra
  entry at index 1, built by `InitPlacedActorEntry` @ 0x0047c010 (which also stores 9 at `+0x04`);
  the 8-slot layout resumes one dword later. Relabelled `PlacedObjectEntryVtbl` with a plate comment.
- **`Map` is consistent, and the multiple-inheritance model got independent confirmation.**
  `MapBase::ClearReferencesTo(void *)` is `RET 0x4` as declared. `RefCountedBase`'s slot 0 is
  0x004727e7, which begins `SUB ECX,0xa4` - an adjustor thunk whose constant is exactly
  `sizeof(MapBase)`. That is the layout `struct Map : MapBase, RefCountedBase` asserts, arrived at
  from the other direction.
- **The `Role` family has nothing to check.** `Destructibility`, `FragData` and
  `ReplaceDestructibility` declare only a virtual destructor, and `Role` itself has no vtable at all.
  Slot 0 is exempt for the reason in §6.3.

### 6.5 Three mirror defects the *first in-game run* turned up

The §6.3/§6.4 sweeps covered **vtable slots**. They could not cover the free functions a mirror
resolves by address, and all three defects below sat in that gap - each one a guaranteed crash that
no amount of building or type-checking would show, and each found within minutes of first exercising
the code in the running game.

- **`GetCommandTable` was missing a dereference** (`src/Console.cpp`). 0x007b6a7c *holds* the bucket
  array; every game reader loads the value (`MOV EAX,[0x007b6a7c]` in `RegisterConsoleCommand`,
  `CommandListCommands`, `FreeCommands`, …). The accessor returned `&CommandTableBuckets` instead, so
  `ForEachConsoleCommand` treated the array pointer as bucket 0's chain head and the neighbouring
  globals as buckets 1..n - `ConsoleTextScrollTarget` @ 0x007b6a80 is a `float`. **`console.commands`
  was a guaranteed access violation**; it now enumerates 257 commands.
- **`HierarchyResolveNamedPointPos` @ 0x00594890 was declared `FastCall`, and it is `__thiscall`
  with two *stack* arguments** (`RET 0x8`). The hierarchy does arrive in ECX, but the out-`Vec3` is
  `[EBP+8]` (it is what the `MOVQ [EBX]` / `MOV [EBX+8]` pair writes) and the node name is
  `[EBP+0xc]` (the second operand of the `stricmp` at 0x005948d2). Declared `__fastcall` the `Vec3`
  went to EDX where nothing reads it, the name landed in the out-pointer slot, and the *name*
  argument came off whatever the caller's stack held. **Every `make.role` with a `hotspot` faulted
  inside `___ascii_stricmp`**; it now resolves (`head` -> `{0, -0.014, 0.073}` on `units\bug.rif`).
- **`MakeCameraTrack` had *two* wrong calls, and the second is the interesting one.** 0x005aa920 is
  now `LoadCameraTrackFromRif`:
  `bool __fastcall(void *track /*ECX*/, void *rif /*EDX*/, const char *name, Vec3 map_origin)` -
  one dword and a **by-value `Vec3`** on the stack, which is the `RET 0x10`. It looks up
  REBENVDT -> SPECLOBJ in the rif, collects every `CUTSHEAD` child and `_stricmp`s `name` against
  each one's `CUTSCDAT` name (+0x28), then hands the match to
  `CameraTrack_LoadFromCutscene` @ 0x005bf060 with `*(float *)rif` (the rif unit scale) and the
  origin. Declared as `FastCall<char, const char *, float, float, float>` the name went to ECX, EDX
  was never set, and 12 bytes were pushed for a callee that pops 16.
  The second defect is that `AcquireLevelRifForLocators` was called **with no argument at all**, on
  the belief that it falls through to the already-loaded level rif. It does not: `ToCameraTrack`
  passes the section's own `file` field (0x01, `parsed+0x240`) in ECX, and the callee `strlen`s it
  with no null check. `CameraTrackDesc` therefore grew a `file` member - the GLS section has always
  had one, and the shipped scripts spell both
  (`camera track { file "levels\level01.rif" name "first contact" }`).
  Verified in a running game: 50 hits and 50 misses interleaved, plus 300 misses in a loop, with the
  process still answering the REPL afterwards and no WER record.

**The lesson is the coverage gap, not the three bugs.** A `RET`-form sweep is cheap and mechanical,
and it was only ever run against vtables. Re-run it over every `GetObjectAtOffset` in `src/`: for a
declared arity, `__fastcall` puts the first two *integral* arguments in registers (floats never) and
`__thiscall` only `this`, so the expected `RET` operand is `4 * stack_args` - and a disagreement is a
defect, not a modelling choice.

#### 6.5.1 That sweep, run over all of `src/`

Every `GetObjectAtOffset` in `src/` (231 sites; 110 distinct address/declaration pairs resolve to a
`.text` function through a convention template) now agrees with its target's `RET` form. **Zero
mismatches.** What it took to get a trustworthy answer:

- **Resolve the declaration per call site, not per name.** `fn` is a local in a dozen files; keying
  on the identifier alone attributes the last declaration seen to every site and produces ~40
  phantom findings.
- **Four tail-jump functions have no `RET` of their own** and must be followed: `ClearCameraTrack`
  @ 0x00487e30 -> 0x00487a00, the `ToRole` thunk @ 0x0047cc10 -> 0x0047cc20, `GetServerTime64`
  @ 0x00505340 -> `ReadScaledClock64` (with ECX preloaded), and `SetIsFogEnabled` @ 0x00472230 ->
  0x004697d0, which **overwrites its own argument slot** before jumping so the tail callee's
  `RET 0x4` is what makes the whole chain `__stdcall`-clean.
- **`"long long".split()[-1]` is `"long"`**, and a size table that misses that silently counts a
  64-bit argument as 4 bytes - which is exactly how `RegisterTriggers`' correct declaration reads as
  a 4-byte mismatch. `TriggerList` is a `List<T>`, 16 bytes by value.

**The `RET` test is blind to register arguments**, which is how the
`AcquireLevelRifForLocators` defect above survived it - a missing ECX argument changes no `RET`
operand. The companion check is: for a target declared with fewer than two register arguments, walk
from its entry and look for a *read* of ECX (or EDX) before any write. Over the 60 such sites it
reports two false positives, both `LEA EDX,[ECX+1]`-shaped writes, and nothing else; run against the
unfixed source it flags 0x00483da0 at `PUSH ECX` and 0x005aa920 at `MOV EAX,EDX`, which is the
self-test that the check can fail at all.

### 6.1 Durable scheduling

`SaveGame` @ 0x00507a80 records every piece of the engine's scheduling: the trigger list (each
record plus its script payload and actor-name list), the `WAIT` and `REAL WAIT` deadlines inside
`SaveSettingsBlock`, the `WAIT FOR` condition (`WriteWaitForState`), the `REPTXT` state, and the
pending `CommandsToExecute` lines.

All of it is **data — a deadline plus a name**. That is exactly why it can be saved, and it is the
constraint any script-side scheduler runs into: a JavaScript closure cannot go in a file. So

- a **time trigger with a message payload** is durable. It survives a save/load and a process
  restart, because what comes back is the payload's `kind` — a string — and the script's side is
  rebuilt by module evaluation;
- anything held in the QuickJS heap is not, and is worse than merely lost: a savegame load rewinds
  the game clock, so an in-heap timer holding an absolute deadline would fire at the wrong moment
  rather than not at all.

One trap in the obvious pattern. The host boots once at `SetupMenus` and a savegame load does not
restart it, so a module-scope `Promise.withResolvers()` is *not* re-created by loading. Load a save
from before the trigger fired and the trigger is restored and fires again, but resolving an
already-settled promise is a no-op — the awaiting code never runs a second time. Keeping the
progress in a token instead does not have that failure mode.


## 5. Defects this inventory turned up

Four, all in GkPlus rather than in the game, and all found by checking a mirror against the
accesses the binary actually makes:

- **`Cheats` was two `int`s**, but `IsGodMode` @ 0x007b9c70 and `IsInfiniteAmmo` @ 0x007b9c71 are
  adjacent **bytes** — every access in the binary is `byte ptr`. As `int`s, `IsInfiniteAmmo`
  landed on 0x007b9c74, which is `RenderStateFlags`: writing the cheat corrupted the renderer's
  state word. Now two bytes with a `static_assert`.
- **`GetEPWEnabled`/`SetEPWEnabled` read and wrote an `int`** at 0x006a3001, a **byte**. The read
  picked up two padding bytes and the low byte of `selected_actor_id` @ 0x006a3004; the write
  clobbered it.
- **`SetCameraDistance` and `SetMaxCameraDistance` each wrote one of two globals.** The game keeps
  a target and a current value for both and writes both whenever it snaps — `CameraDistance1`
  @ 0x007b3e78 / `CameraDistance2` @ 0x007b4e30, `MaxCameraDist1` @ 0x006a5748 /
  `MaxCameraDist2` @ 0x007b9d18. Writing only the first leaves a value the interpolator overwrites.
- **`SetCameraPosition` never rebuilt the view matrix**, which `SET CAMERA POS` does. The camera
  moved only once something else happened to rebuild it.

And one modelling error: `GetConsoleCommandList()` pointed at `CommandsToExecute`, the command
*queue*, and described it with a linked-list struct that matches neither that nor the registry.
Replaced by the hash-table model in §2.
