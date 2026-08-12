# Launch Gunlok for testing and get all the way to a usable REPL.
#
# The REPL port is not chosen here. GKPLUS_REPL_PORT=0 makes the game bind an
# ephemeral port and post the result back to the message-only window GKPort
# opens - see the contract in src/Repl.h. Picking a number instead is a
# race (anything can take the port between the check and the game's bind) and it
# is what forced the old "kill every gl.exe and hope" preamble, since two runs
# could not share one hardcoded 9222. Concurrent instances are fine now, and the
# `pid` in the reply is what proves the port belongs to the game just launched
# rather than to a survivor.
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

// The receiving half of the rendezvous in src/Repl.h: a message-only window of
// class "GkPlusLauncher" that takes one posted "GkPlusReplPort" message, pid in
// wParam and port in lParam.
public static class GKPort {
  public delegate IntPtr WndProc(IntPtr h, uint m, IntPtr w, IntPtr l);

  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
  struct WNDCLASSEX {
    public uint cbSize, style;
    public WndProc lpfnWndProc;
    public int cbClsExtra, cbWndExtra;
    public IntPtr hInstance, hIcon, hCursor, hbrBackground;
    public string lpszMenuName, lpszClassName;
    public IntPtr hIconSm;
  }
  [StructLayout(LayoutKind.Sequential)]
  struct MSG {
    public IntPtr hwnd; public uint message; public IntPtr wParam, lParam;
    public uint time; public int x, y;
  }

  [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  static extern ushort RegisterClassExW(ref WNDCLASSEX c);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  static extern IntPtr CreateWindowExW(int ex, string cls, string name, int style,
      int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  static extern IntPtr DefWindowProcW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  static extern uint RegisterWindowMessageW(string name);
  [DllImport("user32.dll")] static extern bool DestroyWindow(IntPtr h);
  [DllImport("user32.dll")] static extern bool PeekMessageW(out MSG m, IntPtr h, uint lo, uint hi, uint remove);
  [DllImport("user32.dll")] static extern IntPtr DispatchMessageW(ref MSG m);
  [DllImport("user32.dll")] static extern bool ChangeWindowMessageFilterEx(IntPtr h, uint msg, uint action, IntPtr change);

  static readonly IntPtr HWND_MESSAGE = new IntPtr(-3);

  // The delegate has to outlive the window: the class holds a raw pointer to it,
  // so letting it be collected turns the first message into a call through freed
  // memory. A static field is what keeps it rooted.
  static WndProc Proc;
  static uint PortMessage;
  public static IntPtr Window = IntPtr.Zero;
  public static int Port = 0;
  public static int Pid = 0;

  static IntPtr OnMessage(IntPtr h, uint msg, IntPtr w, IntPtr l) {
    if (PortMessage != 0 && msg == PortMessage) {
      // Zero-extended from the 32-bit game, so both fit; the double cast keeps
      // the narrowing unchecked rather than risking an OverflowException.
      Pid = (int)(long)w; Port = (int)(long)l;
      return IntPtr.Zero;
    }
    return DefWindowProcW(h, msg, w, l);
  }

  public static IntPtr Open() {
    Port = 0; Pid = 0;
    if (Window != IntPtr.Zero) return Window;
    Proc = OnMessage;
    PortMessage = RegisterWindowMessageW("GkPlusReplPort");
    var wc = new WNDCLASSEX();
    wc.cbSize = (uint)Marshal.SizeOf(typeof(WNDCLASSEX));
    wc.lpfnWndProc = Proc;
    wc.lpszClassName = "GkPlusLauncher";
    // 0 is ERROR_CLASS_ALREADY_EXISTS on a re-run in the same session, which is
    // harmless - the class is still there and still points at Proc.
    RegisterClassExW(ref wc);
    Window = CreateWindowExW(0, "GkPlusLauncher", "GkPlusLauncher", 0,
                             0, 0, 0, 0, HWND_MESSAGE, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
    // UIPI drops a registered message arriving from a lower integrity level and
    // does it silently, so an elevated shell launching the game normally would
    // just never see the port. MSGFLT_ALLOW = 1.
    ChangeWindowMessageFilterEx(Window, PortMessage, 1, IntPtr.Zero);
    return Window;
  }

  // A *posted* message reaches the window procedure only through DispatchMessage,
  // so this is not optional - without it the port sits in the queue forever. It
  // costs the game nothing to be late, though: nothing on its side is waiting.
  public static void Pump() {
    MSG m;
    while (PeekMessageW(out m, IntPtr.Zero, 0, 0, 1 /* PM_REMOVE */)) DispatchMessageW(ref m);
  }

  public static void Close() {
    if (Window != IntPtr.Zero) { DestroyWindow(Window); Window = IntPtr.Zero; }
    Port = 0; Pid = 0;
  }
}
'@

$GunlokDir = "C:\Program Files (x86)\Steam\steamapps\common\Gunlok"

function Start-Gunlok {
    param(
        [string]$Renderer = "d3d9",      # "vulkan" or "d3d9"
        [switch]$Validation,
        [switch]$Fullscreen,             # answer No to the windowed prompt
        [int]$Port = 0                   # 0 = the OS picks it and the game reports it back
    )
    Stop-Process -Name gl -Force -ErrorAction SilentlyContinue
    # Not about the port any more - an ephemeral one cannot collide with a survivor. A
    # leftover gl.exe still holds d3d8.dll against `cmake --build --target copy`, and
    # Focus-Gunlok takes whichever process it finds first, so one at a time it is.
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Process gl -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
    }
    if (Get-Process gl -ErrorAction SilentlyContinue) { throw "a gl.exe refuses to die" }
    Start-Sleep -Seconds 1

    $env:GKPLUS_REPL_PORT = "$Port"
    $env:GKPLUS_LAUNCHER_HWND = [string][int64][GKPort]::Open()
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

    # The game publishes the port only once the listener is accepting, so this is the
    # readiness signal as well as the number - there is no gap where a connect could
    # arrive first, which the old "retry TcpClient until it succeeds" loop could not say.
    # The message is posted, so giving up is entirely this side's call: the game cannot
    # tell us that nothing is coming.
    $deadline = (Get-Date).AddSeconds(90)
    while (-not [GKPort]::Port -and (Get-Date) -lt $deadline) {
        [GKPort]::Pump()
        Start-Sleep -Milliseconds 200
    }
    if (-not [GKPort]::Port) { throw "the REPL listener never reported a port" }
    # A port from some other gl.exe is worse than no port: every later query would read
    # the wrong game and nothing would look wrong.
    if ([GKPort]::Pid -ne $proc.Id) {
        throw "the port came from gl.exe $([GKPort]::Pid), not $($proc.Id)"
    }
    $global:GunlokReplPort = [GKPort]::Port

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

function Repl([string]$code, [int]$Port = $global:GunlokReplPort) {
    $c = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $Port)
    $s = $c.GetStream(); $s.ReadTimeout = 120000
    $w = New-Object System.IO.StreamWriter($s); $w.NewLine = "`n"; $w.AutoFlush = $true
    $r = New-Object System.IO.StreamReader($s)
    $w.WriteLine($code); $line = $r.ReadLine(); $c.Close()
    return $line
}

# Unwraps the REPL's doubly-encoded reply for `JSON.stringify(...)` expressions.
function ReplJson([string]$code, [int]$Port = $global:GunlokReplPort) {
    $v = (Repl $code $Port | ConvertFrom-Json).value
    return $v.Trim('"').Replace('\"', '"').Replace('\\', '\') | ConvertFrom-Json
}
