# Capture the Gunlok window to a PNG.
#
# PrintWindow rather than CopyFromScreen: the game window can be partially covered, and a screen
# grab would capture whatever is on top of it.
#
# THE FLAG IS 3, NOT 2. PW_RENDERFULLCONTENT (2) is what makes it work at all for a window whose
# contents come from a GPU swapchain - with it absent the bitmap is solid black, because there is
# no WM_PRINT redraw for the compositor to ask for. But **PW_CLIENTONLY (1) has to be there too**:
# without it PrintWindow renders the whole window, title bar and border included, into a bitmap
# that is sized from GetClientRect - so the frame is pushed down and right by the border and the
# bottom and right edges of the game's own picture fall off the bitmap entirely. That is silent:
# the shot looks like a screenshot of the game in its window, and it is missing ~40 rows of what
# was being measured. Found while chasing §4.47, where the whole question was where a draw lands.
#
# Usage:  . .\shot-gunlok.ps1 ; Get-GunlokShot out.png

Add-Type -AssemblyName System.Drawing

Add-Type @'
using System; using System.Runtime.InteropServices;
public static class Shot {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, IntPtr e);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@ -ErrorAction SilentlyContinue

# THE CLIENT RECT IS NOT THE FRAME UNLESS THIS PROCESS IS DPI AWARE. gl.exe is not, so Windows
# hands *it* virtualized coordinates - 418x312 here - while its swapchain is the real 628x468.
# PrintWindow renders at the window's own resolution, so a bitmap sized from a virtualized
# GetClientRect silently keeps only the top-left two thirds. The HUD lives in the upper RIGHT,
# and it was absent from every screenshot of a whole session before anyone noticed; the scene
# looked complete, which is exactly what makes it dangerous.
#
# A DPI-aware caller asking about a non-aware window gets the PHYSICAL rect, so one call fixes
# it. It is per-process and one-way, which is fine for a test shell.
[Shot]::SetProcessDPIAware() | Out-Null

function Get-GunlokShot([string]$Path) {
    $gl = Get-Process gl -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $gl) { throw "gl.exe is not running" }
    $h = $gl.MainWindowHandle

    # The window has to be foreground for the swapchain to have presented recently; the ALT tap
    # releases the foreground lock the same way launch-gunlok.ps1 does.
    [Shot]::keybd_event(0x12, 0, 0, [IntPtr]::Zero)
    [Shot]::keybd_event(0x12, 0, 2, [IntPtr]::Zero)
    [Shot]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 600

    $r = New-Object Shot+RECT
    [Shot]::GetClientRect($h, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $ht = $r.B - $r.T
    if ($w -le 0 -or $ht -le 0) { throw "window has no client area" }

    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc()
    [Shot]::PrintWindow($h, $dc, 3) | Out-Null
    $g.ReleaseHdc($dc)
    $g.Dispose()
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return "$Path ($w x $ht)"
}
