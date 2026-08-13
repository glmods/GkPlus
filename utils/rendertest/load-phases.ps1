# Time each level-load phase by the message the player is shown.
#
#     . .\utils\rendertest\load-phases.ps1
#     Start-Gunlok -Renderer vulkan
#     Measure-LoadPhases -Level level01           # starts it over the REPL
#     Watch-LoadPhases                            # or: drive the menus yourself and watch
#
# Reads the phase out of the process rather than hooking anything.
# `ShowLoadingMessage` @ 0x004e2910 takes its argument in ECX and stores it to the global
# 0x007b6dc4 before doing anything else - `testl %ecx,%ecx` / `cmovnel %ecx,%eax` /
# `movl %eax,0x7b6dc4` - so ReadProcessMemory answers "which phase" from outside. That matters
# for two reasons: the main thread does not pump for most of a load, so the REPL cannot be
# asked; and this works against any build, including one without GkPlus's own instrumentation.
#
# The global is never cleared, so time spent by a phase that sets no message of its own is
# charged to the previous one. There are SEVEN messages in glres<lang>.dll ("Loading AI and UI",
# "Loading sounds", "Loading level data", "Loading textures", "Loading miscellaneous data",
# "Loading shadows", "Loading script files") and a given load shows only some of them.

. "$PSScriptRoot\launch-gunlok.ps1"

Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class LP {
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, int size, out IntPtr read);
  [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, IntPtr e);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
'@ -ErrorAction SilentlyContinue

function Get-LoadPhase {
    param([System.Diagnostics.Process]$Process, [IntPtr]$Slot)
    $p = New-Object byte[] 4; $n = [IntPtr]::Zero
    if (-not [LP]::ReadProcessMemory($Process.Handle, $Slot, $p, 4, [ref]$n)) { return $null }
    $ptr = [BitConverter]::ToUInt32($p, 0); if ($ptr -eq 0) { return $null }
    $s = New-Object byte[] 64
    if (-not [LP]::ReadProcessMemory($Process.Handle, [IntPtr][int64]$ptr, $s, 64, [ref]$n)) { return $null }
    $z = [Array]::IndexOf($s, [byte]0); if ($z -lt 0) { $z = 64 }
    return [Text.Encoding]::ASCII.GetString($s, 0, $z)
}

# Reports each phase as it ends. Presses space periodically to get past the briefing screen,
# which is where a menu-driven load waits; -NoSpace if you are driving that yourself.
function Watch-LoadPhases {
    param([int]$Seconds = 120, [switch]$NoSpace)
    $gl = Get-Process gl -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $gl) { throw "no gl.exe" }
    # gl.exe is relocated, so the slot has to be computed from the live base rather than from
    # the 0x00400000 the addresses in the notes are written against.
    $slot = [IntPtr]([int64]$gl.MainModule.BaseAddress + (0x007b6dc4 - 0x00400000))

    $sw = [Diagnostics.Stopwatch]::StartNew()
    $last = Get-LoadPhase $gl $slot; $lastAt = 0.0; $quiet = 0; $out = @()
    while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
        $now = Get-LoadPhase $gl $slot
        if ($now -ne $last) {
            $ms = [int]($sw.Elapsed.TotalMilliseconds - $lastAt)
            if ($last) { $out += [pscustomobject]@{ phase = $last; ms = $ms } }
            $last = $now; $lastAt = $sw.Elapsed.TotalMilliseconds; $quiet = 0
        } else { $quiet++ }
        if (-not $NoSpace -and ($quiet % 300) -eq 299) {
            [LP]::SetForegroundWindow($gl.MainWindowHandle) | Out-Null
            [LP]::keybd_event(0x20, 0, 0, [IntPtr]::Zero); [LP]::keybd_event(0x20, 0, 2, [IntPtr]::Zero)
        }
        if ($sw.Elapsed.TotalSeconds -gt 8 -and $quiet -gt 80000) { break }
    }
    return $out
}

# Kicks the level off over the REPL and watches. `levels.start` and the Choose Level menu were
# measured against each other and agree to within 2%, so this is representative of the menu path
# even though it skips it.
function Measure-LoadPhases {
    param([string]$Level = "level01", [int]$Seconds = 120)
    Repl "levels.start({script: `"$Level.gls`", console: `"$Level.gcs`"})" | Out-Null
    return Watch-LoadPhases -Seconds $Seconds
}
