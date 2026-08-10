# Load a level and shoot it once the CAMERA HAS STOPPED MOVING, rather than after a fixed delay.
#
# A fixed settle is not comparable across renderers on any level with a camera sequence: the two
# run at different frame rates, so the same wall-clock delay lands at a different point in the
# sweep. junkyard at 20 s gave a close-up under Vulkan and a wide shot under d3d9 with the camera
# globals reading identically a minute later - the frames were from different moments, not from
# different renderers.
#
# Polling the camera until it repeats is renderer-independent by construction: whatever the frame
# rate, "at rest" is the same world state. Verified on junkyard, where both renderers settle to
# the same five values.

. "$PSScriptRoot\launch-gunlok.ps1"
. "$PSScriptRoot\shot-gunlok.ps1"

Add-Type @'
using System; using System.Runtime.InteropServices;
public static class Key {
  [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, IntPtr e);
}
'@ -ErrorAction SilentlyContinue

function Get-Camera([int]$Port = 9222) {
    return Repl 'JSON.stringify({p: camera.position, yaw: camera.yaw, pitch: camera.pitch, roll: camera.roll, d: camera.distance})' $Port
}

# Presses space until the level is actually running, which dismisses the briefing screen
# levels.start lands on. Without it the level loads but never starts - render.draws sits at ~4
# draws a frame and every counter reads like a broken renderer rather than like a game waiting
# for a keypress.
#
# A retry loop and not one press: when the briefing appears depends on how long the load took, so
# a single press at a fixed delay lands before it exists about half the time. `actors.count` is
# the test rather than a draw count, because it is renderer-independent - it reads 0 on the
# briefing screen and 173 once level02 is up.
function Dismiss-Briefing([int]$TimeoutSeconds = 60) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $count = (Repl 'actors.count' | ConvertFrom-Json).value
        if ([int]$count -gt 0) { return [int]$count }
        Focus-Gunlok
        [Key]::keybd_event(0x20, 0, 0, [IntPtr]::Zero)
        [Key]::keybd_event(0x20, 0, 2, [IntPtr]::Zero)
        Start-Sleep -Seconds 2
    }
    throw "the level never started"
}

# Waits until the WORLD PASS is actually submitting geometry, which `actors.count` does not say.
#
# An overlay screen REPLACES the world submit rather than drawing over it (rendering_notes.md §5),
# so the HUD, the objectives text and the pause indicator render over a black frame while the level
# is up and running. That photographs exactly like a renderer drawing nothing, and every A/B taken
# in it reads zero differing pixels - which is indistinguishable from "the knob under test is
# inert" and cost a session concluding exactly that about a build where it was not (notes §4.67).
#
# Neither check already here sees it: `actors.count` read 178 throughout and the renderer was
# healthy at 16.6 ms/frame. The tell is the draw count - 16 this frame against a 273 peak.
#
# **The count has to come from `render.frame_draws`, not `render.draws`.** The latter is
# `vulkan::FormatDrawStats()` - the VULKAN renderer's own counter - so under `-Renderer d3d8` or
# `-Renderer d3d9` it reads "world pipeline: down / draws: 0 this frame" forever and this function
# always threw. Every reference capture the harness was supposed to take was unreachable, which is
# a special case of the trap this whole function exists for: a counter that reads zero because the
# thing counting is switched off looks exactly like a renderer drawing nothing. `frame_draws` is
# mirror-side (see FormatFrameDraws in D3D8Capture.h), so it reads the same in all three modes, and
# its first line is already the scalar.
function Wait-World([int]$TimeoutSeconds = 90, [int]$MinDraws = 100) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $now = -1
    while ((Get-Date) -lt $deadline) {
        $report = Repl 'render.frame_draws(0, 0)'
        if ($report -match '(\d+) draws in the last complete frame') { $now = [int]$Matches[1] }
        if ($now -ge $MinDraws) { return $now }
        # Space again: the usual cause is an overlay the briefing dismissal opened rather than
        # closed, and the same key clears it.
        Focus-Gunlok
        [Key]::keybd_event(0x20, 0, 0, [IntPtr]::Zero)
        [Key]::keybd_event(0x20, 0, 2, [IntPtr]::Zero)
        Start-Sleep -Seconds 2
    }
    throw "the world pass never came up - render.draws stuck at $now a frame, so the shot would be the HUD over black"
}

function Wait-CameraRest([int]$TimeoutSeconds = 150, [int]$StableReads = 3) {
    $last = ""; $same = 0
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2
        $now = Get-Camera
        if ($now -eq $last) { $same++ } else { $same = 0 }
        $last = $now
        if ($same -ge $StableReads) { return $now }
    }
    throw "the camera never came to rest"
}

# One renderer, one level, one screenshot taken at rest and paused.
function Shoot-Settled {
    param(
        [Parameter(Mandatory)][string]$Renderer,
        [Parameter(Mandatory)][string]$Level,
        [Parameter(Mandatory)][string]$Out,
        [string]$Before = "",   # JS run once the level is up and at rest, before the shot
        [switch]$NoPause
    )
    Start-Gunlok -Renderer $Renderer | Out-Null
    Repl "levels.start({script: `"$Level.gls`", console: `"$Level.gcs`"})" | Out-Null
    Start-Sleep -Seconds 5
    Dismiss-Briefing | Out-Null
    Wait-World | Out-Null
    $camera = Wait-CameraRest
    if ($Before -ne "") { Repl $Before | Out-Null; Start-Sleep -Seconds 3 }
    if (-not $NoPause) { Repl 'screen.toggle_pause()' | Out-Null }
    Start-Sleep -Milliseconds 1200
    # An absolute path is used as given; a bare name lands beside these scripts.
    $path = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $PSScriptRoot $Out }
    Get-GunlokShot $path | Out-Null
    return $camera
}

# --- is the RENDERER alive, and what does a frame cost ----------------------------
#
# Two traps, both of which produced a wrong conclusion in one session (notes §4.66):
#
#   * `render.stats.frames` is the CAPTURE LAYER's Present count and keeps climbing whether or not
#     Vulkan is alive - every call is still forwarded to d3d8to9. On a lost device it read
#     10,109 -> 10,456 over four seconds while the renderer's own counter sat frozen.
#   * ... and on the renderer's own counter, "it advanced" is not the test either. A reset-and-
#     restart loop advanced it five frames in eight seconds and was read as survival, against a
#     healthy 16.63 ms/frame.
#
# So this returns a RATE, and calls anything an order of magnitude off `-Healthy` dead.
function Measure-Frame {
    param([int]$Seconds = 8, [double]$Healthy = 100.0)
    $a = [int]((Repl 'render.vulkan.frames_presented' | ConvertFrom-Json).value)
    Start-Sleep -Seconds $Seconds
    $b = [int]((Repl 'render.vulkan.frames_presented' | ConvertFrom-Json).value)
    if ($b -le $a) { return [pscustomobject]@{ Alive = $false; MsPerFrame = [double]::PositiveInfinity; Frames = 0 } }
    $ms = 1000.0 * $Seconds / ($b - $a)
    return [pscustomobject]@{ Alive = ($ms -lt $Healthy); MsPerFrame = [math]::Round($ms, 2); Frames = ($b - $a) }
}
