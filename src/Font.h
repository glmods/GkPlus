#pragma once

#include <cstdint>

// The engine's text layer: AWAPI's `Font` and the queue it lays text into.
//
// **`Font_QueueText` draws nothing.** It lays the string out - wrapping, scrolling,
// clipping, the glyph-substitution pass - and appends a `TextDrawItem` node to the
// font's own pending list at `font+0xb08`. Once per frame, near the end of
// `RenderSceneAndPresent`, `ScenePass_Overlay2D` @ 0x00578ee0 walks the font registry
// and drains every font's queue into the glyph emitter. So text is a **third retained
// path**, parallel to the render queue and invisible to anything hooking
// `RenderQueue_Submit` / `RenderQueue_Flush`.
//
// Two consequences for anything calling in here:
//
//   * **A queued string lives for exactly one frame.** The flush frees each item as it
//     draws it. Text that should stay on screen has to be queued again every frame.
//   * **Queue from the main (client) thread only.** The list has no lock, and the flush
//     runs on the main thread; the executor thread must not touch it. See
//     threading_model_notes.md.
//
// Full analysis in rendering_notes.md 4.2; the addresses are in address_map.md under
// "Text rendering".

namespace gk {

// AWAPI's font object - 0xb18+ bytes, deliberately **not** mirrored. Its layout is
// measured (rendering_notes.md 4.2) but nothing here needs a field out of it: the one
// value a caller wants, the line height, comes from the engine's own accessor. Kept
// incomplete so no static_assert can quietly go stale against a struct nobody checks.
struct Font;

// Normalized 0..1 screen rectangle - the engine's own `RECTF`. `Font_QueueText` scales
// `left`/`right` by ResolutionWidthF and `bottom` by ResolutionHeightF, and `top` by
// ResolutionHeightF **plus a half-unit bias** that the other three do not get.
//
// `bottom` never reaches the queued item: it is purely the vertical limit that
// TextFlags::ClipToBottom truncates against.
struct TextRect {
  float left = 0.0f;
  float top = 0.0f;
  float right = 1.0f;
  float bottom = 1.0f;
};

// The layout/render flags, recovered bit by bit from the two consumers. Four are acted
// on by the queue function, the other five are copied into the item and acted on by the
// renderer at flush time - which is why they are one enum rather than two.
enum class TextFlags : int {
  None = 0x000,
  // Lay out and measure, but emit no item. The one flag that makes a call draw nothing
  // at all; a caller passing a null colour and target must pass this.
  MeasureOnly = 0x001,
  AlignCenter = 0x002,
  // Draws glyph 0x40 in ConsoleCursorColor at index ConsoleCursorPos. The console's
  // caret, and unrelated to LastCharAltColor.
  ConsoleCursor = 0x004,
  // Skip the wrap/scroll pass entirely.
  NoLayout = 0x008,
  // The **final character only** is drawn in `alt_color` rather than `color`.
  LastCharAltColor = 0x010,
  // The glyph is drawn four extra times at +-1px diagonals in TEXT_OUTLINE_COLOR
  // (0xff000000), i.e. a black outline. Five draws per glyph, so it is not free.
  Outline = 0x020,
  // Grow upwards: subtract one line height from the emitted y per line. What puts the
  // version stamp on the bottom edge.
  AnchorBottom = 0x040,
  // Truncate at the last line above `rect.bottom`. **Only honoured when AnchorBottom is
  // clear** - the queue tests the pair as `(flags & 0xc0) == 0x80`.
  ClipToBottom = 0x080,
  // AlignCenter wins if both are set. Tested by the renderer but passed as a literal by
  // no call site in the game.
  AlignRight = 0x100,
};

constexpr TextFlags operator|(TextFlags a, TextFlags b) {
  return static_cast<TextFlags>(static_cast<int>(a) | static_cast<int>(b));
}
constexpr TextFlags &operator|=(TextFlags &a, TextFlags b) { return a = a | b; }
constexpr bool HasFlag(TextFlags flags, TextFlags bit) {
  return (static_cast<int>(flags) & static_cast<int>(bit)) != 0;
}

// The four fonts `InitConsole` builds, in the order their globals sit at
// 0x007b6a54/58/5c/60. Every one is registered into the font list the per-frame flush
// walks, so all four are equally drawable.
//
// **All four are constructed with a line height of 25.** The one that measures 50 is
// `Heading`, and that comes from `Font.scale` (+0xaf4), which `Font_Ctor` sets to 1.0 for
// every font and which exactly one instruction in the binary ever overwrites -
// `ScaleFontsForClientWidth` @ 0x004d79f0, for this font only, at 2.0/2.5/3.0 by client
// width. So the difference is a scale factor applied after construction, not a different
// font size.
//
// `Large` and `Heading` are **the same font twice**: identical texture, identical width
// table, identical line height, differing only in that scale. And neither has anything to
// do with the console despite the names these carried until the textures were read - the
// console draws exclusively through `Small`.
enum class FontId {
  // `bitmaps\small font.RIM`. The game's default UI text: the console, both menu systems,
  // the order menu, the inventory, the briefing body text and the version stamp. 32 of the
  // 39 call sites.
  Small,
  // `bitmaps\large font.RIM` - a 512x512 sheet against Small's 256x256, so higher texel
  // density rather than a bigger glyph box, with ~26% wider advances. The emphasis font:
  // loading messages, the credits screen, one menu heading.
  Large,
  // `bitmaps\small font 2.RIM`. The in-game HUD: the HUD itself, the target-info panel,
  // the targeting reticule, the inventory item panel. Same metrics as Small, narrowest
  // advances of the four.
  Hud,
  // `bitmaps\large font.RIM` again, at 2x-3x (50-75px). The oversized heading of the
  // full-screen text pages, used only for lines flagged "big" on the briefing,
  // training-debrief and stats pages.
  Heading,
};

// --- Native API --------------------------------------------------------------------

// The font behind `id`, or null before `InitConsole` has run (i.e. before the game has
// finished starting up). Borrowed - the game owns these for the process lifetime.
Font *GetFont(FontId id);

// Line height as a fraction of screen height: `(line_height * scale) /
// ResolutionHeightF`, via Font_GetNormalizedLineHeight @ 0x005782b0. Zero for a null
// font. This is the unit `TextRect`'s vertical members are in, which is what makes
// "one line above the bottom edge" expressible.
//
// The `scale` half is why FontId::Heading measures double everything else despite all
// four being constructed at 25 - so ask rather than assuming 25/ResolutionHeightF.
float LineHeight(Font *font);

// The render target the menus and most overlays queue against - the object
// `UpdateAndDrawMenuScreen` installs into the current-target slot. `QueueText` uses it
// when `target` is null, which is what a caller with no opinion wants.
void *DefaultTextTarget();

// The longest string `QueueText` will pass to the engine. `Font_QueueText` copies the
// caller's text into a **1028-byte stack buffer with no bound** and smashes its own
// frame past that (game_defects_notes.md 1), so this wrapper truncates rather than
// forwarding a string that would corrupt the stack. 1027 characters plus the NUL.
inline constexpr int kMaxTextLength = 0x403;

// One string to queue. Everything except `text` has a working default, and the two
// pointer members are the ones worth reading twice.
struct TextDraw {
  TextRect rect;
  const char *text = nullptr;
  // D3DCOLOR, passed to the engine by address; the queue dereferences it once and
  // stores the value, so nothing here is retained.
  unsigned color = 0xffffffffu;
  // Null means DefaultTextTarget(). Unlike `color` this **is** retained: the item holds
  // it until the flush, which compares it against the font's current target and breaks
  // the render batch when it changes. Pass a game object, never a temporary.
  void *target = nullptr;
  // Characters to lay out; <= 0 means the whole string. The engine's own clamp is an
  // *unsigned* compare, so a negative value there means "no limit" rather than "none" -
  // QueueText normalises that rather than passing it through.
  int max_chars = 0;
  TextFlags flags = TextFlags::None;
  // 0..1 between the target camera's near and far z planes, becoming the vertex z with
  // rhw = k/z. 0.0 is nearest, which is what an overlay wants.
  float depth = 0.0f;
  // Only read under TextFlags::LastCharAltColor, and only if non-null.
  const unsigned *alt_color = nullptr;
  // Scroll offset: start the emitted text at this line and shift y up accordingly.
  int skip_lines = 0;
};

// Queue one string for this frame's overlay pass. Returns the engine's own result - the
// number of lines laid out, or 0 when it clipped - and 0 without calling anything for a
// null font or null text.
//
// The caller keeps ownership of `draw.text`: this copies whatever the engine needs
// before returning, so a temporary buffer is fine. Text longer than kMaxTextLength is
// **truncated**, not refused; see that constant for why.
int QueueText(Font *font, const TextDraw &draw);

// Replaces the game's bottom-left version stamp with GkPlus's own.
//
// `DrawVersionText` @ 0x004f72e0 draws the hardcoded literal "v1.3 DX8" and is called
// from exactly two places - the Main menu draw, gated on `ChosenMenu == MenuId::Main`,
// and the pre-menu splash frame. Both are replaced, so the stamp reads
// "GkPlus - <renderer>" wherever the game would have shown its own, in whatever colour
// that call site chose. Nothing else on screen is touched.
//
// `GKPLUS_VERSION_TEXT=raw` restores the stock string, the same escape hatch
// WindowPlacementSystem offers.
//
// RAII: attaches in the ctor, detaches in the dtor - construct and destroy inside a
// Detours transaction.
class VersionTextSystem {
public:
  VersionTextSystem();
  ~VersionTextSystem();
};

} // namespace gk
