#pragma once

// Throttling the loading screen's repaint.
//
// --- why this exists ------------------------------------------------------------
//
// A level load presents once per role it loads - ~830 times for level12, measured
// with the profiler's frame ring. The load's wall clock is therefore roughly
// `presents x cost-of-one-present`, and that makes it hostage to the present mode:
//
//   d3d8, unthrottled   ~1.2 ms a present  ->  ~1.0 s
//   Vulkan, FIFO        16.7 ms a present  ->  ~13.9 s
//
// Both numbers are measured, and the second is why a load that takes a second on
// one renderer takes thirteen on the other while doing identical work. The load is
// not slower; it is waiting for 830 vertical blanks to draw a progress bar.
//
// So a present that arrives during a load is **dropped** unless enough time has
// passed since the last one that was shown. ~30 Hz keeps the bar animating and
// takes 830 presents down to a few dozen, which makes the load cost what the load
// costs on either renderer.
//
// --- what "during a load" means -------------------------------------------------
//
// `LoadLevel` @ 0x004e0980 being on the stack, via the one detour this installs.
// It is `__fastcall(bool freshStart)` - the flag is spilled by
// `movb %cl, -0x175(%ebp)` at 0x004e09ad, so there are no stack arguments and the
// function is caller-clean (`level_loading_notes.md` section 2.2).
//
// GameState == 0x12 was the alternative and is not used: `BeginLevelSession` sets
// it before `LoadLevel` and something else clears it afterwards, so its edges are
// not the load's edges, and throttling presents into live gameplay would be
// visible. A detour has exactly the right extent by construction.
//
// `GKPLUS_LOAD_PRESENT_MS` sets the interval; `raw` or `0` disables the throttle.

namespace gk {
namespace loadscreen {

// True while LoadLevel is on the stack.
bool Loading();

// Whether the present now being asked for should be dropped instead of shown.
// Pure - call NotePresented() after one that actually happened. False whenever no
// load is running, so the cost outside a load is one load and a branch.
bool SuppressPresent();

// Records that a present was shown, restarting the interval.
void NotePresented();

// Records that one was dropped instead, for the per-load tally.
void NoteDropped();

// RAII, like every other *System: construct inside a Detours transaction from
// entry.cpp.
class LoadScreenSystem {
public:
  LoadScreenSystem();
  ~LoadScreenSystem();
  LoadScreenSystem(const LoadScreenSystem &) = delete;
  LoadScreenSystem &operator=(const LoadScreenSystem &) = delete;
};

} // namespace loadscreen
} // namespace gk
