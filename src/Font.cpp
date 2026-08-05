#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Font.h"

#include "Core.h"
#include "D3D8Capture.h"
#include "DetourUtils.h"

#include <cstddef>
#include <cstring>
#include <iterator>
#include <string>

namespace gk {
namespace {

// The four fonts InitConsole builds, in the order FontId lists them - which is the
// order of the globals, and deliberately *not* the order they are constructed in:
// construction #3 stores into 0x007b6a60 (Heading) and #4 into 0x007b6a5c (Hud).
constexpr uintptr_t FontGlobals[] = {0x007b6a54, 0x007b6a58, 0x007b6a5c, 0x007b6a60};

// The render target the menus and most overlays queue against - the object
// UpdateAndDrawMenuScreen installs into the current-target slot. The call sites push
// this address itself, so the global *is* the object rather than a pointer to it.
constexpr uintptr_t DefaultTargetObject = 0x007b50b0;

// Font_QueueText @ 0x005782e0. `rect`, `text`, `color` and `alt_color` are all borrowed
// for the call only - it copies the string into its own pool block (malloc @ 0x005787f4
// plus an inline strcpy) and dereferences the two colours once. `target` is the
// exception: it is stored raw and read again at flush time.
using QueueTextFn = ThisCall<int, Font *, const TextRect *, const char *,
                             const unsigned *, void *, int, int, float,
                             const unsigned *, int>;

// Font_GetNormalizedLineHeight @ 0x005782b0. Writes through `out` and returns it in
// EAX; we use the out parameter, which is the half that does not depend on the return
// convention being right.
using LineHeightFn = ThisCall<float *, Font *, float *>;

// DrawVersionText @ 0x004f72e0. __fastcall with the colour in ECX - it is an argument,
// not a `this`: the function forwards it to Font_QueueText as the colour pointer.
using DrawVersionTextFn = FastCall<void, unsigned *>;

DrawVersionTextFn OriginalDrawVersionText = nullptr;
bool VersionTextHooked = false;
bool VersionTextEnabled = true;

// What the stamp reads. Built on first use rather than at construction because the
// renderer is not resolved until Direct3DCreate8 runs, which is long after DllMain -
// asking at hook-install time would always answer "d3d9".
const char *VersionText() {
  static const std::string text = std::string("GkPlus - ") + d3d8::RendererName();
  return text.c_str();
}

// Replaces the game's own "v1.3 DX8" everywhere it would have appeared, keeping the
// call site's colour and position. Both callers - the Main menu and the splash frame -
// pass a colour and expect nothing back, so there is nothing else to preserve.
//
// The original is deliberately not called: it would queue "v1.3 DX8" underneath ours in
// the same place, and the overlay pass draws both.
void __fastcall HookedDrawVersionText(unsigned *color) {
  Font *font = GetFont(FontId::Small);
  if (!font) {
    // Before InitConsole there is nothing to draw with. The original would fault on the
    // same null, so forwarding is not obviously safer - but it is the game's own
    // behaviour, which is the right default for a path we do not expect to reach.
    if (OriginalDrawVersionText) {
      OriginalDrawVersionText(color);
    }
    return;
  }

  // The stock rect, reproduced: one line tall, hugging the bottom-left, 1% in from the
  // left edge with its baseline at 99% of screen height.
  TextDraw draw;
  draw.rect.left = 0.01f;
  draw.rect.right = 1.0f;
  draw.rect.bottom = 0.99f;
  draw.rect.top = 0.99f - LineHeight(font);
  draw.text = VersionText();
  draw.color = color ? *color : 0xffffffffu;
  draw.flags = TextFlags::AnchorBottom;
  QueueText(font, draw);
}

// GKPLUS_VERSION_TEXT = gkplus (default) | raw.
void ReadVersionTextMode() {
  char value[32] = {};
  const DWORD len =
      ::GetEnvironmentVariableA("GKPLUS_VERSION_TEXT", value, sizeof(value));
  const std::string mode(value, len);
  if (mode.empty() || mode == "gkplus") {
    return;
  }
  VersionTextEnabled = false;
  if (mode != "raw") {
    DebugWrite("gkplus: unknown GKPLUS_VERSION_TEXT '" + mode +
               "'; leaving the game's own version stamp alone\n");
  }
}

} // namespace

Font *GetFont(FontId id) {
  const auto index = static_cast<size_t>(id);
  if (index >= std::size(FontGlobals)) {
    return nullptr;
  }
  Font **slot = nullptr;
  GetObjectAtOffset(slot, FontGlobals[index]);
  return *slot;
}

float LineHeight(Font *font) {
  if (!font) {
    return 0.0f;
  }
  LineHeightFn fn;
  GetObjectAtOffset(fn, 0x005782b0);
  float height = 0.0f;
  fn(font, &height);
  return height;
}

void *DefaultTextTarget() {
  void *target = nullptr;
  GetObjectAtOffset(target, DefaultTargetObject);
  return target;
}

int QueueText(Font *font, const TextDraw &draw) {
  if (!font || !draw.text) {
    return 0;
  }

  // The clamp that makes this safe to expose. Font_QueueText copies the caller's string
  // into a 1028-byte stack buffer with no bound and smashes its own frame past that
  // (game_defects_notes.md 1) - the game hits this itself on the training-level debrief.
  // Truncating is the one behaviour that cannot regress anything, since a longer string
  // currently corrupts the stack rather than rendering.
  const size_t length = std::strlen(draw.text);
  std::string truncated;
  const char *text = draw.text;
  if (length > static_cast<size_t>(kMaxTextLength)) {
    truncated.assign(draw.text, static_cast<size_t>(kMaxTextLength));
    text = truncated.c_str();
    DebugWrite("gkplus: text of {} characters truncated to {} before queueing - the "
               "engine's layout buffer is 1028 bytes and unbounded\n",
               length, kMaxTextLength);
  }

  // The engine's own clamp is `min_unsigned(max_chars, strlen)`, so a negative value
  // there reads as "no limit" rather than "none". Normalise instead of passing it
  // through: a caller asking for -1 characters means the whole string either way, and a
  // caller asking for 0 means none, which the engine already honours.
  int max_chars = draw.max_chars;
  if (max_chars <= 0) {
    max_chars = static_cast<int>(std::strlen(text));
  }

  QueueTextFn fn;
  GetObjectAtOffset(fn, 0x005782e0);
  const unsigned color = draw.color;
  return fn(font, &draw.rect, text, &color,
            draw.target ? draw.target : DefaultTextTarget(), max_chars,
            static_cast<int>(draw.flags), draw.depth, draw.alt_color,
            draw.skip_lines);
}

VersionTextSystem::VersionTextSystem() {
  ReadVersionTextMode();
  if (!VersionTextEnabled) {
    return;
  }
  GetObjectAtOffset(OriginalDrawVersionText, 0x004f72e0);
  ::DetourAttach(reinterpret_cast<void **>(&OriginalDrawVersionText),
                 reinterpret_cast<void *>(HookedDrawVersionText));
  VersionTextHooked = true;
}

VersionTextSystem::~VersionTextSystem() {
  if (!VersionTextHooked) {
    return;
  }
  ::DetourDetach(reinterpret_cast<void **>(&OriginalDrawVersionText),
                 reinterpret_cast<void *>(HookedDrawVersionText));
  VersionTextHooked = false;
}

} // namespace gk
