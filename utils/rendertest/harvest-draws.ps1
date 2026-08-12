# Accumulate a per-texture render-state profile out of the capture layer's draw log.
#
# `render.frame_draws()` holds only the LAST COMPLETE frame, so a single dump is one
# camera's worth of draws. This merges every frame into a running per-texture map
# *inside the game* and brings back only the totals, which is what makes a whole
# session's worth of evidence affordable over a socket.
#
# There is no C++ change and no `main.mjs`: the REPL evaluates in global scope and
# `var`/`function` persist between lines, so the accumulator lives in the REPL
# context's own globals and nothing under <Gunlok>\gkplus is touched.
#
#   . .\utils\rendertest\harvest-draws.ps1
#   Start-Gunlok -Renderer d3d9
#   Open-Repl ; Seed-Harvest
#   Harvest-Level level02 ; Harvest-Level level03
#   Save-Harvest C:\somewhere\harvest.json
#
# then, from pbr\:  uv run python -m gkpbr.cli observed --from C:\somewhere\harvest.json
#
# Five things this encodes, on top of what launch-gunlok.ps1 and shoot-settled.ps1
# already do, each of which cost a run to find:
#
#   * The dedup is on `render.stats.frames`, which is incremented in
#     CaptureDevice::Present in the same call that swaps DrawLogLastFrame - so
#     (frames == N) pairs exactly with "the log holds frame N", and polling faster
#     than the frame rate costs nothing. Measured: 166,922 frames sampled, 64 missed
#     while sampling.
#   * ONE socket, kept open. `Repl` from launch-gunlok.ps1 opens and closes a
#     connection per call, which caps the poll rate well below the ~50 fps the game
#     runs at and starts losing frames.
#   * The camera is driven with the ARROW KEYS, not by assigning `camera.position`.
#     GLkeys.cfg binds scroll to the bare arrows, rotate/zoom to the same arrows with
#     a modifier and elevation to Z/X; setting the position directly puts the camera
#     somewhere the level does not exist and the frame goes black, which looks
#     exactly like a renderer fault. `camera.center_on` is no help either - CENTRE
#     takes a unit *number* and answers "Invalid (negative) unit number" to a name.
#   * Do NOT press Escape at the top-level main menu. It exits the game (a clean
#     exit, so GLkeys.cfg is rewritten) and takes the accumulator with it.
#   * Four multiplayer maps kill the game on `levels.start`, and it IS a crash with a
#     WER dump - it only looks like a wedge because the REPL socket dies with the
#     process. `mplay_bombsite`, `mplay_canyon`, `mplay_dockyard` and
#     `mplay_tf_oilrig01` are dev leftovers the exe never lists, and they #include
#     unit headers whose .RIF was never shipped; that is game_defects_notes.md
#     section 8, a null Role::hierarchy in the game's own ToRole. Not our fault and
#     not fixable from here - just do not harvest them.
#     The seven the exe DOES list all work: mplay_atlantic, mplay_carpark,
#     mplay_machine, mplay_mountain, mplay_rorschasch, mplay_warehouse, mplay_zorro.
#     Only atlantic has a .map sidecar, so the other six read as Responding=False for
#     ~10 s on first load while they build one. Campaign levels and Maze are fine,
#     cutscene or not: a cutscene only matters when a test asserts on world state,
#     and for coverage a flying camera is a bonus.

. "$PSScriptRoot\launch-gunlok.ps1"

Add-Type @'
using System; using System.Runtime.InteropServices;
public static class HK {
  [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, IntPtr e);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
}
'@ -ErrorAction SilentlyContinue

$script:HConn = $null
$script:HWriter = $null
$script:HReader = $null

function Open-Repl([int]$Port = $global:GunlokReplPort) {
    Close-Repl
    $script:HConn = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $Port)
    $script:HConn.NoDelay = $true
    $stream = $script:HConn.GetStream()
    $stream.ReadTimeout = 120000
    $script:HWriter = New-Object System.IO.StreamWriter($stream)
    $script:HWriter.NewLine = "`n"
    $script:HWriter.AutoFlush = $true
    $script:HReader = New-Object System.IO.StreamReader($stream)
}

function Close-Repl {
    if ($script:HConn) { try { $script:HConn.Close() } catch {} }
    $script:HConn = $null
}

function Rx([string]$code) {
    $script:HWriter.WriteLine($code)
    return $script:HReader.ReadLine()
}

function RxVal([string]$code) {
    $obj = (Rx $code) | ConvertFrom-Json
    if (-not $obj.ok) { throw "REPL error on '$code': $($obj.error)" }
    return $obj.value
}

# Multi-line source has to ride in the object form: a newline is the frame delimiter.
function RxBlock([string]$source) {
    return Rx (@{ code = $source } | ConvertTo-Json -Compress)
}

# The accumulator. `frame_draws` returns the report text, whose columns are
#   idx type prims fvf from blend src dst z zw cull atest depth "x,y" "WxH" texture
# and the texture is everything from token 15 on, because a .rim path may contain
# spaces ("ground/rock and moss") while the viewport rect is always two tokens.
$HarvestSeed = @'
var HV = HV || null;
function hvreset() {
  HV = {t: {}, frames: 0, polls: 0, draws: 0, notex: 0, level: "?", last: -1,
        badlines: 0, missed: 0, levels: {}};
  return "ok";
}
function hvlevel(name) { HV.level = name; HV.last = -1; return HV.level; }
function hvburst() { HV.last = -1; return HV.frames; }
function hv() {
  var f = render.stats.frames;
  HV.polls++;
  if (f === HV.last) return 0;
  if (HV.last >= 0 && f > HV.last + 1) HV.missed += f - HV.last - 1;
  HV.last = f;
  HV.frames++;
  HV.levels[HV.level] = (HV.levels[HV.level] || 0) + 1;
  var lines = render.frame_draws().split("\n");
  var n = 0;
  for (var i = 2; i < lines.length; i++) {
    var line = lines[i];
    if (!line) continue;
    var p = line.split(" ").filter(function (s) { return s.length; });
    if (p.length < 15) { HV.badlines++; continue; }
    n++;
    var tex = p.slice(15).join(" ").toLowerCase();
    if (!tex) { HV.notex++; continue; }
    var e = HV.t[tex];
    if (!e) e = HV.t[tex] = {d: 0, p: 0, s: {}, f: {}, L: {}, v: {}};
    e.d++;
    e.p += (+p[2] || 0);
    var st = p[5] + "," + p[6] + "," + p[7] + "," + p[8] + "," + p[9] + "," + p[10] + "," + p[11];
    e.s[st] = (e.s[st] || 0) + 1;
    e.f[p[3]] = (e.f[p[3]] || 0) + 1;
    e.L[HV.level] = (e.L[HV.level] || 0) + 1;
    var vp = p[13] + " " + p[14];
    e.v[vp] = (e.v[vp] || 0) + 1;
  }
  HV.draws += n;
  return n;
}
function hvstat() {
  return JSON.stringify({textures: Object.keys(HV.t).length, frames: HV.frames,
                         polls: HV.polls, draws: HV.draws, notex: HV.notex,
                         badlines: HV.badlines, missed: HV.missed, level: HV.level});
}
var HVDUMP = "";
function hvfreeze() { HVDUMP = JSON.stringify(HV); return HVDUMP.length; }
function hvchunk(i) { return HVDUMP.substr(i * 12000, 12000); }
'@

function Seed-Harvest {
    RxBlock $HarvestSeed | Out-Null
    RxVal 'hvreset()' | Out-Null
}

function Poll-Frames([double]$Seconds) {
    Rx 'hvburst()' | Out-Null
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) { Rx 'hv()' | Out-Null }
}

# A REPL reply over 64 K characters is elided with a note, and `value` is display
# text (JSON.stringify of the result), so the dump is frozen once and read in
# chunks small enough to survive being escaped a second time.
function Save-Harvest([string]$Path) {
    $len = [int](RxVal 'hvfreeze()')
    $sb = New-Object System.Text.StringBuilder
    for ($i = 0; $i -lt [math]::Ceiling($len / 12000.0); $i++) {
        [void]$sb.Append(((RxVal "hvchunk($i)") | ConvertFrom-Json))
    }
    [System.IO.File]::WriteAllText($Path, $sb.ToString())
    return $sb.Length
}

function KeyDown([byte]$vk) { [HK]::keybd_event($vk, [byte][HK]::MapVirtualKey($vk, 0), 0, [IntPtr]::Zero) }
function KeyUp([byte]$vk)   { [HK]::keybd_event($vk, [byte][HK]::MapVirtualKey($vk, 0), 2, [IntPtr]::Zero) }

function Hold([byte]$vk, [double]$Seconds, [byte[]]$Mods = @()) {
    foreach ($m in $Mods) { KeyDown $m }
    KeyDown $vk
    Poll-Frames $Seconds
    KeyUp $vk
    foreach ($m in $Mods) { KeyUp $m }
}

$VK_LEFT = 0x25; $VK_UP = 0x26; $VK_RIGHT = 0x27; $VK_DOWN = 0x28
$VK_CONTROL = 0x11; $VK_SPACE = 0x20; $VK_Z = 0x5A; $VK_X = 0x58

function Camera-Tour([double]$Leg = 2.5) {
    Focus-Gunlok
    foreach ($pass in 0, 1) {
        Hold $VK_LEFT  $Leg
        Hold $VK_UP    $Leg
        Hold $VK_RIGHT ($Leg * 2)
        Hold $VK_DOWN  ($Leg * 2)
        Hold $VK_LEFT  $Leg
        Hold $VK_UP    $Leg
        Hold $VK_LEFT  $Leg @($VK_CONTROL)    # rotate
        Hold $VK_RIGHT $Leg @($VK_CONTROL)
        Hold $VK_UP    1.0  @($VK_CONTROL)    # zoom in
        Hold $VK_DOWN  1.5  @($VK_CONTROL)    # zoom out
        Hold $VK_X     1.0                    # elevate
        Hold $VK_Z     1.0
    }
}

# Space until actors.count is non-zero, per shoot-settled.ps1: when the briefing
# appears depends on how long the load took, so one press at a fixed delay misses it
# about half the time. Best-effort here rather than fatal - a map with no player
# roster never reaches a non-zero count and its draws are worth having anyway.
function Dismiss-BriefingHarvesting([int]$TimeoutSeconds = 45) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        Poll-Frames 1.5
        $count = [int](RxVal 'actors.count')
        if ($count -gt 0) { return $count }
        Focus-Gunlok
        KeyDown $VK_SPACE; Start-Sleep -Milliseconds 60; KeyUp $VK_SPACE
    }
    return -1
}

function Wait-Rest([int]$TimeoutSeconds = 45, [int]$Stable = 3) {
    $last = ""; $same = 0
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        Poll-Frames 2
        $now = RxVal 'JSON.stringify([camera.position, camera.yaw, camera.pitch, camera.distance])'
        if ($now -eq $last) { $same++ } else { $same = 0 }
        $last = $now
        if ($same -ge $Stable) { return $now }
    }
    return $last
}

#: Effects that need no named object, so they run in any level. The liquid family
#: (WATER/LAVA/OIL/SEA/SWAMP) does need one -- two dummy objects naming a rectangle --
#: and is issued per level by hand; see the README.
$FxNoArgs = @('RATE 100', 'RAIN 1', 'SNOW 1', 'LIGHTNING 1')

function Harvest-Level {
    param([Parameter(Mandatory)][string]$Level, [double]$Leg = 2.5)

    RxVal "levels.start({script: `"$Level.gls`", console: `"$Level.gcs`"})" | Out-Null
    RxVal "hvlevel(`"$Level/briefing`")" | Out-Null
    Start-Sleep -Seconds 4
    Poll-Frames 3
    $n = Dismiss-BriefingHarvesting
    RxVal "hvlevel(`"$Level`")" | Out-Null
    Wait-Rest | Out-Null
    Poll-Frames 3
    Camera-Tour $Leg
    RxVal "hvlevel(`"$Level/fx`")" | Out-Null
    foreach ($c in $FxNoArgs) {
        try { RxVal "console.execute(`"$c`")" | Out-Null } catch { "  fx failed: $c" }
    }
    Poll-Frames 6
    return "$Level : actors $n ; " + (RxVal 'hvstat()')
}
