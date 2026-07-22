#include "InputFix.h"

#include "Core.h"
#include "DetourUtils.h"

namespace gk {
namespace {
// The game builds a DirectInput SysKeyboard device at startup
// (SetupDInputKeyboard @ 0x004e4090), Acquire()s it, re-Acquire()s it on every
// alt-tab refocus (ReacquireDInputKeyboard @ 0x004e3ce0, via OnActivateApp), and
// Unacquire()s it at shutdown - but nothing ever reads it. The only function
// that drains its buffered data (PollDInputKeyboard @ 0x004e3d40) has no callers
// or pointer references anywhere in the image; real keystrokes reach the game
// through Win32 messages: WM_KEYDOWN -> MainWindowWndProc -> HandleKeyMessage
// (0x004e3f20) -> HandleKeyPress4, with TranslateVkToScanCode (0x004e4300)
// turning the VK into the same DIK scan code DirectInput would have produced.
// The whole DirectInput keyboard is a fossil of an abandoned input path (so is
// the DirectInput joystick reader alongside it).
//
// Its one live consequence is the WH_KEYBOARD_LL low-level hook that dinput.dll
// arms inside this process on Acquire(). While the game is frozen in a debugger
// that hook can't answer, so Windows serializes every system-wide keypress
// behind LowLevelHooksTimeout (~300ms each). Suppressing the Acquire() keeps the
// hook from ever being armed, with no gameplay effect - the mouse is unaffected
// because it runs on Raw Input (InitializeMouseRawInput @ 0x004e42c0), not
// DirectInput.
//
// AcquireDInputDevice @ 0x004e3cb0 is the single call site of
// IDirectInputDevice::Acquire (vtbl+0x1c), covering both the startup and the
// refocus paths. It is __fastcall with only the device pointer in ECX and no
// stack arguments, so a member function pointer models it exactly - the same
// trick Music.cpp uses for the __fastcall MusicTrack constructor.
struct DInputDevice {
  int HookedAcquire();
};
int (DInputDevice::*AcquireDInputDevice)() = nullptr;
} // namespace

// Report success without ever forwarding to the real Acquire(). SetupDInputKeyboard
// checks the result >= 0 and ReacquireDInputKeyboard ignores it, so returning 1
// satisfies both callers while the device is left unacquired (and thus hookless).
int DInputDevice::HookedAcquire() { return 1; }

InputFixModule::InputFixModule(lua_State *L) : Module{L} {
  GetObjectAtOffset(AcquireDInputDevice, 0x004e3cb0);
  DetourAttach(&AcquireDInputDevice, &DInputDevice::HookedAcquire);
}

InputFixModule::~InputFixModule() {
  DetourDetach(&AcquireDInputDevice, &DInputDevice::HookedAcquire);
}
} // namespace gk
