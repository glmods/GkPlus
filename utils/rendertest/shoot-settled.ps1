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
    $camera = Wait-CameraRest
    if ($Before -ne "") { Repl $Before | Out-Null; Start-Sleep -Seconds 3 }
    if (-not $NoPause) { Repl 'screen.toggle_pause()' | Out-Null }
    Start-Sleep -Milliseconds 1200
    # An absolute path is used as given; a bare name lands beside these scripts.
    $path = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $PSScriptRoot $Out }
    Get-GunlokShot $path | Out-Null
    return $camera
}
