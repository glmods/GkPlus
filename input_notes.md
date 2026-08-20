# Gunlok Input System - Reverse Engineering Notes

## Overview

Gunlok reads the **keyboard through Win32 window messages** and the **mouse through Raw
Input**. It *also* creates a DirectInput `SysKeyboard` device, but that device is a
**fossil of an abandoned input path**: it is acquired, re-acquired on focus, and released
on shutdown, yet **nothing ever reads it**. The one live consequence of that dead device
is the `WH_KEYBOARD_LL` low-level hook `dinput.dll` arms on `Acquire()`, which freezes
system-wide keyboard input whenever the process is paused in a debugger. GkPlus's
`gk.inputfix` module ([src/InputFix.cpp](src/InputFix.cpp)) suppresses that acquire.

Everything funnels into one dispatcher, `HandleKeyPress4` @ 0x0046a360, which takes a
`KeyPressData` and fans out to `HandleKeyPress2` -> `HandleKeyPress3` /
`HandleConsoleKeyPress`. Keyboard keys, mouse buttons, mouse movement and the mouse wheel
are all expressed as `KeyPressData` records distinguished by the `state` field.

```
Keyboard (LIVE)                            Mouse (LIVE)
===============                            ============
OS WM_KEYDOWN/WM_KEYUP/WM_SYS*             OS WM_INPUT (RIM_TYPEMOUSE)
  -> MainWindowWndProc  0x0046aad0           -> MainWindowWndProc 0x0046aad0
  -> HandleKeyMessage   0x004e3f20           -> OnRawInput_WmInput 0x004e3ea0
     (VkToScanCodeTable, KeyModifierState)   -> ProcessRawMouseButtons 0x004e4310
  -> HandleKeyPress4    0x0046a360         WinMain per-frame
                                             -> DispatchMouseMove 0x004e39b0
DirectInput keyboard (VESTIGIAL)             (MouseXPixels/MouseYPixels)
================================           -> HandleKeyPress4 0x0046a360
InitInputDevices 0x004e3bf0
  -> SetupDInputKeyboard 0x004e4090        common sink:
     CreateDevice/SetDataFormat/           HandleKeyPress4 -> HandleKeyPress2
     SetCooperativeLevel(FG|NONEXCL)/         -> HandleKeyPress3
     SetProperty(BUFFERSIZE=32)/              -> HandleConsoleKeyPress
     QueryInterface/Acquire  <-- arms the
                                 WH_KEYBOARD_LL hook; data never read
```

## Live keyboard path (Win32 messages)

`MainWindowWndProc` @ 0x0046aad0 handles `WM_KEYDOWN`/`WM_SYSKEYDOWN` (state = `Pressed`)
and `WM_KEYUP`/`WM_SYSKEYUP` (state = `Released`) by calling `HandleKeyMessage`
@ 0x004e3f20 with the state in ECX.

`HandleKeyMessage`:
1. Reads the current virtual-key code and translates it via `TranslateVkToScanCode`
   @ 0x004e4300, which is just `return VkToScanCodeTable[vk]` - a 256-entry `int` LUT at
   0x006a72f8 mapping Win32 VK codes to **DIK / PS-2 Set-1 scan codes** (the `KeyScanCode`
   enum). This is why the message path produces exactly the scan codes DirectInput would.
   Sample entries: `VK_ESCAPE 0x1b -> 0x01`, `A 0x41 -> 0x1e`, `SPACE 0x20 -> 0x39`,
   `RETURN 0x0d -> 0x1c`, `LEFT 0x25 -> 0xcb`, `UP 0x26 -> 0xc8`, `F1 0x70 -> 0x3b`.
2. Modifier keys (`KEY_CTRL`/`KEY_LShift`/`KEY_Alt`) do **not** dispatch; they update the
   global modifier bitmask `KeyModifierState` @ 0x007b6ddc (`MOD_Shift 1`, `MOD_Ctrl 2`,
   `MOD_Alt 4`) - set on `Pressed`, cleared on `Released`.
3. Every other key builds a `KeyPressData{state, scanCode, modifiers = KeyModifierState}`
   and calls `HandleKeyPress4`.

## Live mouse path (Raw Input)

`InitializeMouseRawInput` @ 0x004e42c0 registers a single `RAWINPUTDEVICE{usUsagePage=1,
usUsage=2 (mouse), dwFlags=0, hwndTarget=NULL}`. Because `hwndTarget` is NULL, `WM_INPUT`
is delivered to the focused window -> `MainWindowWndProc`. **Only the mouse is registered
for raw input; the keyboard is not.**

- `OnRawInput_WmInput` @ 0x004e3ea0 (WndProc case `WM_INPUT`, 0xFF): calls
  `GetRawInputData(RID_INPUT)` twice (size, then data); if `RAWINPUTHEADER.dwType ==
  RIM_TYPEMOUSE (0)` it calls `ProcessRawMouseButtons`.
- `ProcessRawMouseButtons` @ 0x004e4310: reads `RAWMOUSE.usButtonFlags` and emits a
  `KeyPressData` per button transition with **<400ms double-click detection** (double-click
  sets modifier bit 0x1000; per-button timestamps live at 0x007b6e30). Button states use
  `KeyState` literals 0x201/0x204/0x207 (down), 0x202/0x205/0x208 (up).
- `DispatchMouseMove` @ 0x004e39b0: called per-frame from `WinMain` (and 0x004b0a30). If
  the accumulated pixel position `MouseXPixels`/`MouseYPixels` (0x006b02ac / 0x006b02b0)
  changed since last frame, it dispatches a **`state = 0x200` (move)** `KeyPressData`.
- The mouse wheel arrives as a registered `WM_MOUSEWHEEL` in the WndProc and dispatches
  `state = 0x20a`.

### Where a mouse button actually goes: the `msg - 0x201` table

Raw Input supplies the *button transitions*, but the routing to game behaviour happens back on the
message side, and it was not recorded here. `HandleKeyPress3` @ 0x00470990 carries a jump table at
**0x00470c3c** indexed by `msg - 0x201`, which selects one of **three interface registry objects —
one per mouse button** — and calls a thin wrapper that supplies it as `this`:

| registry | button | down wrapper | up wrapper |
|---|---|---|---|
| `LeftButtonInterface` @ 0x007b41f8 | left | `OnLeftButtonDown` @ 0x00496e30 | `OnLeftButtonUp` @ 0x00496e50 |
| `RightButtonInterface` @ 0x007b4498 | right | `OnRightButtonDown` @ 0x00496f30 | `OnRightButtonUp` @ 0x00496f50 |
| `MiddleButtonInterface` @ 0x007b3f88 | middle | `OnMiddleButtonDown` @ 0x00496e80 | `OnMiddleButtonUp` @ 0x00496ea0 |

Each wrapper tail-calls the shared dispatchers `InterfaceRegistry_OnButtonDown` @ 0x004a25d0 and
`InterfaceRegistry_OnButtonUp` @ 0x004a2700 (the latter computes a **20-unit drag threshold in
640x400 space** and then picks the drag or click matrix). Those dispatchers index a **6x7 handler
matrix by `[cursor target class][CursorMode]`** — the full mechanism, including why the matrix
dimensions were documented backwards for a while, is in `orders_notes.md` §8.5. The point for *this*
file is that the six wrappers above are the boundary: everything upstream is input plumbing,
everything downstream is cursor-mode-dependent game behaviour.

### `HandleGameKeyAction` is an if-else chain over `.data`, not a switch

`HandleGameKeyAction` @ 0x0046f700 is reached from `HandleKeyPress3` @ 0x00470b43 (gated at
0x00470b2c on `ConsoleStatus != 0`, i.e. the console is not capturing) and requires
`event->[0x00] == 0x100` (`WM_KEYDOWN`). It then runs a **linear if-else chain** comparing
`event->[0x10]` (the DIK) and the current modifier state against **pairs of dwords in `.data`**
spanning roughly 0x007b72xx-0x007b74xx — one `{key, modifier_mask}` pair per bindable action. There is
no jump table, so the number of comparisons is the number of bindings.

Each pair is installed by `RegisterKeyBinding` @ 0x004f7360, which stores `[EDI] = key`
(0x004f737b) and `[EDI+4] = modifier` (0x004f7380) — exactly the two dwords the chain compares — and
files the binding into a `List<T>` at **0x007b74f0 + category*0x10**, which is what the Controls
screen enumerates. All of them are registered by `RegisterAllKeyBindings` @ 0x004effc0, called from
`WinMain`.

A worked example, because it settled a standing "is this feature dead?" question: the **flare** key.

```
004f0521  PUSH 0x2 / PUSH 0x0 / PUSH 0x21          ; category 2, modifier 0, DIK 0x21 = DIK_F
004f0527  MOV EDX,0x2340                           ; GL_CONTROLS_FIREFLARE
004f052c  MOV ECX,0x725664 / CALL GetResourceString
004f0538  MOV ECX,0x7b73cc / CALL RegisterKeyBinding
```

and the matching arm of the chain:

```
0046ffe4  CMP ESI,dword ptr [0x007b73cc]   ; the key
0046ffee  JNZ ...
0046ffee  CALL 0x004e3e60                  ; current modifier state
0046fff3  CMP EAX,dword ptr [0x007b73d0]   ; the modifier mask
0046fffb  CALL EnterFlareMode              ; @0x004a17e0
```

That single call site is `EnterFlareMode`'s **only** reference in the whole database, which is what
proves the player flare feature is reachable — a rebindable Controls row, default **F**. Sibling
registrations pin the scancode space (`0x0b` = DIK_0, `0x39` = DIK_SPACE, `0xd3` = DIK_DELETE).

## Data structures

`KeyPressData` (0x1c bytes) - the universal input record fed to `HandleKeyPress4`:

| Offset | Field | Type | Notes |
|--------|-------|------|-------|
| 0x00 | `state` | `KeyState` | `Pressed 0x100` / `Released 0x101`; mouse uses 0x200/0x201.. as literals |
| 0x04 | `wParam` | int | raw message wParam / mouse coords |
| 0x08 | `lParam` | int | raw message lParam / packed mouse x,y |
| 0x0c | (int) | int | |
| 0x10 | `scanCode` | `KeyScanCode` | DIK / Set-1 scan code |
| 0x14 | `modifiers` | `KeyModifier` | snapshot of `KeyModifierState` (+0x1000 dbl-click) |
| 0x18 | (void*) | void* | also reused as the stack cookie slot in callers |

- `KeyState` enum: only `Pressed 0x100` / `Released 0x101` are named; the mouse/wheel
  pseudo-states (0x200 move, 0x201/0x204 button-down, 0x202/0x205 button-up, 0x207/0x208,
  0x20a wheel) appear as **raw literals** in the code, not enum members.
- `KeyModifier` enum: `MOD_Shift 1`, `MOD_Ctrl 2`, `MOD_Alt 4`. Bit 0x1000 = double-click;
  higher bits (0x8..0x400) are used as mouse-button flags by the raw-mouse/joystick code.
- `KeyScanCode` enum: 87 named DIK/Set-1 codes (`KEY_ESC 0x1` .. extended keys 0xc8+).

## Vestigial DirectInput subsystem

`InitInputDevices` @ 0x004e3bf0 = `InitDInput` -> `SetupDInputKeyboard` ->
`EnumDevices(EnumKeyboards)` -> `InitializeMouseRawInput` -> `Release`.

- `InitDInput` @ 0x005a4390: `CoCreateInstance(DirectInputGUID, IDirectInput7AGUID)` +
  `Initialize(0x700)`. DirectInput is used via **COM**, not by linking `dinput.dll`.
- `SetupDInputKeyboard` @ 0x004e4090: `CreateDevice(SysKeyboard)` -> `SetDataFormat` ->
  `SetCooperativeLevel(MainWindowHwnd @ 0x007c1228, 6)` where **6 = `DISCL_FOREGROUND |
  DISCL_NONEXCLUSIVE`** -> `SetProperty(DIPROP_BUFFERSIZE = 32)` (buffered) ->
  `GetDeviceInfo` -> `QueryInterface(IDirectInputDevice2A)` into `DInputKeyboardDevice`
  @ 0x007b6e14 -> `AcquireDInputDevice`.
- `AcquireDInputDevice` @ 0x004e3cb0: the **single** call site of
  `IDirectInputDevice::Acquire` (vtbl+0x1c). `__fastcall`, device pointer in ECX.
  Also reached from `ReacquireDInputKeyboard` @ 0x004e3ce0 on alt-tab refocus
  (`OnActivateApp` @ 0x0046f400, `HasWindowFocus` @ 0x006a3744).
- `ReleaseDInputDevices` @ 0x004e3c40 (shutdown): `Unacquire` (vtbl+0x20) the keyboard /
  joystick / a third slot (`DInputKeyboardDevice`, `DInputJoystickDevice`, 0x007b6e18) and
  `Release` the enumerated array. Note 0x007b6e18 is **released but never created or
  acquired** - a pure fossil.
- `EnumKeyboards` @ 0x004e4190 -> `QueryKeyboardDevice2` @ 0x004e4010: QIs each enumerated
  keyboard to `IDirectInputDevice2A` and stores up to 10 in `DInputKeyboardEnumArray`
  @ 0x007b6de8 (count `DInputKeyboardEnumCount` @ 0x007b6de4). Never acquired -> harmless.

### Dead readers (no call refs *and* no pointer refs anywhere in the image)

| Address | Name | What it would do |
|---------|------|------------------|
| 0x004e3d40 | `PollDInputKeyboard_Unused` | drain `GetDeviceData` (vtbl+0x28) -> `HandleKeyPress4` |
| 0x004e3a30 | `PollDInputJoystick_Unused` | `GetDeviceState` DIJOYSTATE (0x50) -> `KeyModifierState` |
| 0x004e3d00 | `SelectDInputKeyboard_Unused` | pick + acquire an enumerated keyboard |

Evidence the DirectInput keyboard is an abandoned path, not intentional:
1. **Two producers, one sink** - `PollDInputKeyboard_Unused` and the live `HandleKeyMessage`
   build the identical `KeyPressData` and both call `HandleKeyPress4`.
2. **Buffering set up but never drained** - `DIPROP_BUFFERSIZE = 32` only serves
   `GetDeviceData`, which only the dead drain calls.
3. **0x007b6e18** - teardown for a device nothing ever creates.
4. The joystick repeats the same dead-reader + dead-acquire shape.

## The debugger-freeze gotcha and the fix

A **non-exclusive** DirectInput `SysKeyboard` is implemented by `dinput.dll` on top of a
global **`WH_KEYBOARD_LL`** hook, armed inside this process on `Acquire()`. Low-level hooks
are dispatched synchronously in the hook owner's thread with a timeout; when the game is
frozen at a breakpoint that thread never answers, so Windows serializes **every** system
keypress behind `LowLevelHooksTimeout` (default ~300ms; registry
`HKCU\Control Panel\Desktop\LowLevelHooksTimeout`). The mouse is unaffected because Raw
Input installs no hook.

**Fix** (`gk.inputfix`, [src/InputFix.cpp](src/InputFix.cpp)): detour `AcquireDInputDevice`
@ 0x004e3cb0 to `return 1` without forwarding. The device is never acquired, the hook is
never armed, and gameplay input is unchanged (the live keyboard path is `WM_KEYDOWN`).
`SetupDInputKeyboard` checks the result `>= 0` and `ReacquireDInputKeyboard` ignores it, so
reporting success satisfies both callers. Reversible: unloading the module detaches the
detour and the acquire returns to normal.

## Function reference (offsets from base)

| Offset | Name | Role |
|--------|------|------|
| 0x0046aad0 | `MainWindowWndProc` | window proc; keyboard + WM_INPUT + wheel dispatch |
| 0x004e3f20 | `HandleKeyMessage` | WM_KEY* -> scan code + modifier -> HandleKeyPress4 |
| 0x004e4300 | `TranslateVkToScanCode` | `VkToScanCodeTable[vk]` (VK -> DIK) |
| 0x0046a360 | `HandleKeyPress4` | universal input dispatcher (keys, mouse, wheel) |
| 0x0046f1d0 | `HandleKeyPress2` | second-stage dispatch |
| 0x00470990 | `HandleKeyPress3` | third-stage dispatch |
| 0x004d43f0 | `HandleConsoleKeyPress` | console text input |
| 0x004e42c0 | `InitializeMouseRawInput` | RegisterRawInputDevices(mouse) |
| 0x004e3ea0 | `OnRawInput_WmInput` | WM_INPUT handler (mouse) |
| 0x004e4310 | `ProcessRawMouseButtons` | RAWMOUSE buttons + double-click |
| 0x004e39b0 | `DispatchMouseMove` | per-frame mouse-move dispatch |
| 0x004e3bf0 | `InitInputDevices` | top-level input init |
| 0x005a4390 | `InitDInput` | CoCreateInstance IDirectInput7A |
| 0x004e4090 | `SetupDInputKeyboard` | create + acquire SysKeyboard (vestigial) |
| 0x004e4040 | `SetDInputBufferSize` | SetProperty(DIPROP_BUFFERSIZE) |
| 0x004e3cb0 | `AcquireDInputDevice` | **Acquire (vtbl+0x1c) - the hook choke point** |
| 0x004e3ce0 | `ReacquireDInputKeyboard` | re-acquire on refocus |
| 0x004e3c40 | `ReleaseDInputDevices` | shutdown unacquire/release |
| 0x004e4190 | `EnumKeyboards` | DI keyboard enumeration callback |
| 0x004e4010 | `QueryKeyboardDevice2` | QI enumerated device -> array |
| 0x0046f400 | `OnActivateApp` | WM_ACTIVATEAPP focus handler |
| 0x004e3d40 | `PollDInputKeyboard_Unused` | dead keyboard drain |
| 0x004e3a30 | `PollDInputJoystick_Unused` | dead joystick reader |
| 0x004e3d00 | `SelectDInputKeyboard_Unused` | dead device selector |

## Global reference (offsets from base)

| Offset | Type | Name |
|--------|------|------|
| 0x006a72f8 | int[256] | `VkToScanCodeTable` (VK -> DIK LUT) |
| 0x007b6ddc | KeyModifier | `KeyModifierState` (live shift/ctrl/alt bitmask) |
| 0x006b02ac | int | `MouseXPixels` |
| 0x006b02b0 | int | `MouseYPixels` |
| 0x007c1228 | HWND | `MainWindowHwnd` |
| 0x006a3744 | int | `HasWindowFocus` |
| 0x007b6e14 | IDirectInputDevice2A* | `DInputKeyboardDevice` (acquired, never read) |
| 0x007b6e10 | IDirectInputDevice2A* | `DInputJoystickDevice` |
| 0x007b6de8 | void*[10] | `DInputKeyboardEnumArray` |
| 0x007b6de4 | int | `DInputKeyboardEnumCount` |
