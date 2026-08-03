# Launch Gunlok for testing and get all the way to a usable REPL.
#
# Handles the three things that otherwise waste a run:
#   * -skipfmv skips the ~40 s intro FMV, during which Bink presents outside the D3D device
#     so every frame counter reads zero and the renderer looks broken;
#   * -skipfmv also raises a modal "Development only mode requester" dialog (class #32770)
#     asking "Run in a window?". It blocks BEFORE the REPL listener opens, so nothing can be
#     driven until it is answered. Yes (IDYES=6) = windowed, No (IDNO=7) = full screen.
#     Windowed is what the Vulkan renderer and GKPLUS_RENDER_UNFOCUSED both require.
#   * the window has to be focused at least once or a level load sticks at game.state 18.
#
# Usage:  . .\launch-gunlok.ps1 ; Start-Gunlok -Renderer vulkan ; Repl 'actors.count'

Add-Type @'
using System; using System.Text; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class GK {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, IntPtr e);
  public static IntPtr FindDialog(uint want) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, p) => {
      uint pid; GetWindowThreadProcessId(h, out pid);
      if (pid == want) {
        var c = new StringBuilder(64); GetClassNameW(h, c, 64);
        if (c.ToString() == "#32770") { found = h; return false; }
      }
      return true;
    }, IntPtr.Zero);
    return found;
  }
}
'@

$GunlokDir = "C:\Program Files (x86)\Steam\steamapps\common\Gunlok"

function Start-Gunlok {
    param(
        [string]$Renderer = "d3d9",      # "vulkan" or "d3d9"
        [switch]$Validation,
        [switch]$Fullscreen,             # answer No to the windowed prompt
        [int]$Port = 9222
    )
    Stop-Process -Name gl -Force -ErrorAction SilentlyContinue
    # A leftover instance keeps port 9222 bound, so the wait loop below would connect to the
    # OLD process and every later query would read the wrong game.
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Process gl -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
    }
    if (Get-Process gl -ErrorAction SilentlyContinue) { throw "a gl.exe refuses to die" }
    Start-Sleep -Seconds 1

    $env:GKPLUS_REPL_PORT = "$Port"
    $env:GKPLUS_RENDERER = $Renderer
    $env:GKPLUS_RENDER_UNFOCUSED = "1"
    $env:GKPLUS_VK_VALIDATION = $(if ($Validation) { "1" } else { "0" })

    # LunarG no longer ships 32-bit Windows components, so the SDK has no 32-bit validation
    # layer and HKLM\SOFTWARE\WOW6432Node\Khronos\Vulkan\ExplicitLayers is empty. The layer
    # is built from source instead, via vcpkg:
    #     vcpkg install vulkan-validationlayers:x86-windows-static-md
    # (run from a directory with no vcpkg.json, or vcpkg is in manifest mode and refuses).
    # VK_ADD_LAYER_PATH *adds* to the search path; VK_LAYER_PATH would replace it and hide
    # any implicit layers the driver installs.
    if ($Validation) {
        $env:VK_ADD_LAYER_PATH = "C:\Users\franc\GkPlus\vcpkg\installed\x86-windows-static-md\bin"
    }

    $proc = Start-Process -FilePath "$GunlokDir\gl.exe" -WorkingDirectory $GunlokDir `
                          -ArgumentList "-skipfmv" -PassThru

    # The dialog blocks before the REPL opens, so it must be answered first.
    $deadline = (Get-Date).AddSeconds(30); $dlg = [IntPtr]::Zero
    while ($dlg -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        $dlg = [GK]::FindDialog([uint32]$proc.Id)
    }
    if ($dlg -eq [IntPtr]::Zero) { throw "the -skipfmv dialog never appeared" }
    $button = $(if ($Fullscreen) { 7 } else { 6 })
    [GK]::PostMessage($dlg, 0x0111, [IntPtr]$button, [GK]::GetDlgItem($dlg, $button)) | Out-Null

    $deadline = (Get-Date).AddSeconds(90); $up = $null
    while (-not $up -and (Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2
        try { $up = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $Port) } catch {}
    }
    if (-not $up) { throw "the REPL listener never opened" }
    $up.Close()

    Focus-Gunlok
    return $proc
}

# SetForegroundWindow alone is ignored under the foreground lock; the ALT tap releases it.
function Focus-Gunlok {
    # Select-Object -First 1: a leftover instance makes Get-Process return an array, and
    # passing that to SetForegroundWindow throws a conversion error rather than doing
    # anything useful.
    $gl = Get-Process gl -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $gl) { return }
    for ($i = 0; $i -lt 6 -and [GK]::GetForegroundWindow() -ne $gl.MainWindowHandle; $i++) {
        [GK]::keybd_event(0x12, 0, 0, [IntPtr]::Zero)
        [GK]::keybd_event(0x12, 0, 2, [IntPtr]::Zero)
        [GK]::SetForegroundWindow($gl.MainWindowHandle) | Out-Null
        Start-Sleep -Milliseconds 400
    }
}

function Repl([string]$code, [int]$Port = 9222) {
    $c = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $Port)
    $s = $c.GetStream(); $s.ReadTimeout = 120000
    $w = New-Object System.IO.StreamWriter($s); $w.NewLine = "`n"; $w.AutoFlush = $true
    $r = New-Object System.IO.StreamReader($s)
    $w.WriteLine($code); $line = $r.ReadLine(); $c.Close()
    return $line
}

# Unwraps the REPL's doubly-encoded reply for `JSON.stringify(...)` expressions.
function ReplJson([string]$code, [int]$Port = 9222) {
    $v = (Repl $code $Port | ConvertFrom-Json).value
    return $v.Trim('"').Replace('\"', '"').Replace('\\', '\') | ConvertFrom-Json
}
