#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// The CPU profiler. Two sources over one store:
//
//   * **instrumented zones** - GK_ZONE in our own code, giving an exact tree with names;
//   * **a sampling thread** - suspend/GetThreadContext/resume at ~1 kHz, giving a flat
//     profile of where the instruction pointer is, including inside gl.exe, which no amount
//     of instrumentation on our side can reach.
//
// The two are joined rather than parallel: a sample records the *zone the sampled thread was
// inside at the time*, read out of the thread's published slot, so "62% of the samples inside
// ConvertVertices are in memcpy" is one query rather than two profiles held side by side.
//
// **The unit is the frame, not the session.** Every aggregate here is over a window of recent
// frames. That is not a preference: the pathology this was built for is a frame time that is
// 5.13 ms on one run and 17.39 on the next with no knob touched (vulkan_renderer_notes.md
// §4.79), and a running total is exactly the instrument that cannot see it.
//
// **Every frame record carries whether it was throttled**, for the same section's reason: three
// sections of renderer work were measured against a frame waiting for a vertical blank, and the
// rule earned there - read the present mode before quoting a frame time - is a field here rather
// than a discipline. `FrameSummary::throttled` is true under FIFO/FIFO_RELAXED or a D3D
// presentation interval, and the JS surface reports it beside every millisecond it prints.
//
// Threading: the recording path takes no lock and allocates nothing, and is safe from any
// thread. Both game threads record - the executor reaches the upload path (§4.72), which is
// where the leading suspect lives. The read path is main-thread (the script host) and copies
// out; it tolerates being overrun by a writer rather than blocking one.

namespace gk::prof {

// Which instrumentation is live. A zone tests its own category against ActiveMask inline, so a
// category that is off costs a load and a not-taken branch at the call site and nothing else.
//
// Draw is separate from Render because it is per-draw: at ~60 ns a zone and the ~700 draws a
// level frame submits it is 0.04 ms, which is 1% of a 4 ms frame and therefore visible. It is
// off in kDefaultMask on purpose - an instrument that moves what it measures is what §4.79 was
// about.
enum class Cat : uint32_t {
  None = 0,
  Frame = 1u << 0,  // frame boundaries and the top-level phases
  Render = 1u << 1, // the renderer's passes, record and submit
  Upload = 1u << 2, // vertex conversion and the staging ring
  Script = 1u << 3, // the QuickJS host, draw_gui, the REPL
  Io = 1u << 4,     // the VFS and the file hooks
  Game = 1u << 5,   // detoured game functions
  Draw = 1u << 6,   // per-draw; off by default, see above
  All = 0xffffffffu,
};

inline constexpr uint32_t kDefaultMask =
    uint32_t(Cat::Frame) | uint32_t(Cat::Render) | uint32_t(Cat::Upload) |
    uint32_t(Cat::Script) | uint32_t(Cat::Io) | uint32_t(Cat::Game);

// Read by every zone. Relaxed because a zone opening or closing one frame late is not a defect
// worth a fence on the hot path; on x86 a relaxed load is a plain mov.
extern std::atomic<uint32_t> ActiveMask;

// --- recording ----------------------------------------------------------------------------

// Interns a call site. Registered on first execution and never freed, so the id is stable for
// the process and an Event can carry 16 bits instead of a pointer. Thread-safe: it is called
// exactly once per site by a function-local static's guard, and takes a lock to do it.
//
// Registration happens whether or not the profiler is armed, which is what lets `prof.sites`
// list what is instrumented before anything is turned on.
uint16_t RegisterSite(const char *name, Cat cat);

// Opens a zone; returns the start timestamp, or 0 when nothing is recording. Zone's destructor
// tests that value rather than the mask, so a zone opened while armed still closes correctly
// after a disarm - which is what keeps the per-thread depth balanced.
uint64_t Open(uint16_t site);
void Close(uint16_t site, uint64_t begin);

// A zero-duration event at the current depth. For things with no extent worth timing: a level
// load starting, a swapchain rebuild, a script message arriving.
void Instant(uint16_t site);
// A named number sampled at a point in time - bytes uploaded, draws submitted, ring bytes free.
// Plotted against the frame timeline rather than summed.
void Counter(uint16_t site, uint32_t value);

class Zone {
public:
  Zone(uint16_t site, Cat cat) : begin_(0), site_(site) {
    if (ActiveMask.load(std::memory_order_relaxed) & uint32_t(cat)) {
      begin_ = Open(site);
    }
  }
  // Ends the zone before its scope does, for the case where the thing being timed does not
  // nest inside a block - the Vulkan record pass, which runs from vkBeginCommandBuffer to
  // vkEndCommandBuffer inside a much longer function. Idempotent, so the destructor is safe
  // after it.
  void End() {
    if (begin_ != 0) {
      ::gk::prof::Close(site_, begin_);
      begin_ = 0;
    }
  }
  ~Zone() { End(); }
  Zone(const Zone &) = delete;
  Zone &operator=(const Zone &) = delete;

private:
  uint64_t begin_;
  uint16_t site_;
};

#define GK_PROF_CAT2(a, b) a##b
#define GK_PROF_CAT(a, b) GK_PROF_CAT2(a, b)

// The one macro. `name` must be a literal or a pointer that outlives the process - the site
// table stores it rather than copying.
#define GK_ZONE(name, cat)                                                               \
  static const uint16_t GK_PROF_CAT(gk_prof_site_, __LINE__) =                           \
      ::gk::prof::RegisterSite(name, cat);                                               \
  ::gk::prof::Zone GK_PROF_CAT(gk_prof_zone_, __LINE__)(                                 \
      GK_PROF_CAT(gk_prof_site_, __LINE__), cat)

#define GK_ZONE_FN(cat) GK_ZONE(__FUNCTION__, cat)

// --- frames -------------------------------------------------------------------------------

// Closes the current frame and opens the next. Called from CaptureDevice::Present, which is the
// boundary that holds for both renderers - the game's own PresentScene is gated on window focus
// and the Vulkan path returns before the inner Present ever runs.
void FrameMark();

// What the frame is being presented under. Set once by whoever knows - the Vulkan renderer when
// it picks a present mode, the capture layer from the game's presentation interval - and stamped
// onto every frame recorded afterwards. `how` must outlive the process; it is what the JS
// surface prints ("fifo", "immediate", "d3d interval one").
void NotePresentMode(const char *how, bool throttled);

// --- arming -------------------------------------------------------------------------------

struct Config {
  uint32_t mask = kDefaultMask;
  // Events per thread. 65536 is ~5 s of history at 200 zones a frame, and costs 1 MB a thread.
  uint32_t events_per_thread = 65536;
  uint32_t frames = 512;
  bool sampler = true;
  uint32_t sampler_hz = 1000;
  uint32_t samples = 65536;
  // Walk the sampled thread's frame pointers as well as recording its EIP, so a hot leaf can be
  // attributed to a caller. Off by default: it costs `stack_depth` words a sample in a second
  // arena that is not allocated at all while this is false, plus the walk itself inside the
  // suspend window.
  //
  // **gl.exe keeps frame pointers** - measured, and the opposite of what was assumed when this
  // was designed. A walk runs straight through the game: on level02, 92% of samples reach the
  // full depth and the chain reads
  // `BuildDrawRecord <- Aw_DrawIndexedPrimitiveUP <- SubMesh_DrawIndexed <- SceneNode_Render`.
  // So this is worth turning on, not a last resort.
  bool stacks = false;
  // 12 is not enough for level02's render path - `SceneNode_Render` recurses down the scene
  // graph and saturates it - but the first six frames are where the answer is, and every frame
  // costs 4 bytes on every sample. Raise it when the tail is what you are after.
  uint32_t stack_depth = 12; // clamped to kMaxStackFrames

  // The trigger's storage, allocated at Arm so a capture is a memcpy rather than a malloc in
  // the middle of a frame. Four captures of 32768 events and 4096 samples is ~3 MB with stacks
  // on. Zero captures disables the trigger regardless of TriggerConfig::enabled.
  uint32_t captures = 4;
  uint32_t capture_events = 32768;
  uint32_t capture_samples = 4096;
};

// The hard ceiling on a recorded stack. A sample's frames live in their own arena, so raising
// this costs memory only when `stacks` is on.
inline constexpr uint32_t kMaxStackFrames = 32;

// Allocates the rings, publishes the mask and starts the sampler. Idempotent while armed;
// **the rings are allocated once and never freed**, because a recorder may be inside one on the
// other thread at the moment a disarm is asked for and there is no cheap way to know. A second
// Arm with a larger capacity therefore grows, and never shrinks.
//
// Main thread only - it allocates, and the sampler thread it starts must not be created from
// inside a suspended-thread window.
bool Arm(const Config &config);
void Disarm();
bool Armed();
const Config &CurrentConfig();

// Changes which categories record. Remembered for the next Arm either way, but only published
// to ActiveMask while armed - a mask on a disarmed profiler would open zones into rings that do
// not exist yet.
void SetMask(uint32_t mask);

// --- reading ------------------------------------------------------------------------------
//
// All main-thread, all snapshotting. A reader may be overrun by a writer that laps the ring
// mid-copy; that shows up as `lost` rather than as a torn record, because each slice checks the
// writer's head before and after.

struct FrameSummary {
  uint64_t index = 0;
  double ms = 0.0;
  bool throttled = false;
  const char *present = "";
  uint32_t events = 0; // zones recorded inside it, across all threads
  uint32_t samples = 0;
  // Raw clock ticks, for correlating a frame against events and samples. Not milliseconds and
  // not an epoch - only differences against another value from this profiler mean anything.
  uint64_t t_begin = 0;
};

struct ZoneRow {
  const char *name = "";
  uint32_t calls = 0;
  double incl_ms = 0.0;
  double self_ms = 0.0;
  double max_ms = 0.0; // the worst single call in the window
  uint32_t thread = 0; // slot index; see Threads()
};

struct SampleRow {
  uint32_t address = 0; // absolute EIP, resolved to module+rva at format time
  uint32_t samples = 0;
  const char *zone = ""; // the instrumented zone the thread was inside, "" for none
  uint32_t thread = 0;
};

// One distinct call stack and how often it was sampled. `frames` is **leaf first**: [0] is the
// sampled EIP and the rest are its callers, outwards.
struct StackRow {
  std::vector<uint32_t> frames;
  uint32_t samples = 0;
  const char *zone = "";
  uint32_t thread = 0;
};

struct ThreadInfo {
  uint32_t slot = 0;
  uint32_t id = 0;
  const char *name = ""; // "main", "executor", or "thread-<id>"
  uint32_t events = 0;
  uint32_t lost = 0; // events overwritten before a reader took them
};

// What a query covers. Every read below takes one, because "the last N frames" is not the only
// question worth asking and used to be the only one askable: a stutter fifteen seconds ago could
// be *seen* in WorstFrames and not profiled, since narrowing to it was impossible even while its
// samples were still in the ring.
//
// `source` is which storage the window reads: the live rings, or one of the trigger captures.
struct Window {
  uint64_t t_begin = 0;
  uint64_t t_end = 0;
  uint64_t first_index = 0; // frame indices, inclusive
  uint64_t last_index = 0;
  uint32_t frames = 0;
  int32_t capture = -1; // -1 is the live rings
  bool valid = false;
};

// The last `count` complete frames; 0 means every frame still held.
Window LastFrames(uint32_t count);
// `pre` frames before frame `index` through `post` after it, clamped to what is held. This is
// what makes a stutter profilable after the fact.
Window AroundFrame(uint64_t index, uint32_t pre, uint32_t post);
// The whole of capture `index` (see Captures()).
Window CaptureWindow(uint32_t index);

// Newest last. Fewer than `count` when the ring holds fewer.
std::vector<FrameSummary> Frames(uint32_t count);
// The `count` slowest frames in the ring, worst first.
std::vector<FrameSummary> WorstFrames(uint32_t count);
// The frames inside a window, oldest first. For a capture this is the captured frames.
std::vector<FrameSummary> FramesIn(const Window &window);

// Aggregated over `window`. Sorted by self time, descending. Self time is exact: the window is
// rebuilt in start order and walked with a stack, so a zone's children are subtracted whether or
// not they are in the same category.
std::vector<ZoneRow> Zones(const Window &window);
// The flat sample profile over the same window, sorted by count, descending.
std::vector<SampleRow> Samples(const Window &window);

// Distinct call stacks over the same window, most frequent first. Empty unless the sampler was
// armed with `stacks`. `limit` of 0 means all.
std::vector<StackRow> Stacks(const Window &window, uint32_t limit);

std::vector<ThreadInfo> Threads();

// --- the trigger ----------------------------------------------------------------------------
//
// The flight recorder. A stutter rarer than the event ring's reach - which is 10 to 60 seconds,
// and shrinks the faster the game runs - cannot be caught by looking afterwards, because its
// zones have been overwritten by the time anyone notices. So the profiler watches for it itself
// and copies the surrounding window out of the rings into storage they cannot overwrite.
//
// **Unthrottle before using this.** Under FIFO the frame time is quantized to the refresh
// interval: a 6 ms hitch is absorbed entirely and a 20 ms one reads as one extra interval, so
// both the threshold and the baseline are measuring the monitor. `GKPLUS_VK_PRESENT_MODE=
// immediate`.

struct TriggerConfig {
  bool enabled = false;
  // A frame fires the trigger when it exceeds **both**. The floor stops a fast steady game from
  // tripping on ordinary jitter; the multiple stops one absolute number from being wrong for
  // every scene, since a menu frame and a level frame differ by 4x before anything goes wrong.
  double min_ms = 20.0;
  double multiple = 4.0;
  // Frames of context kept either side. `post` is not padding: a stutter's cause often shows in
  // the recovery - a ring wrap, a reload, a re-bake - and the snapshot is therefore deferred by
  // this many frames rather than taken the moment the trigger fires.
  uint32_t pre = 120;
  uint32_t post = 30;
};

struct CaptureInfo {
  uint32_t index = 0;        // position in Captures(), which is what CaptureWindow takes
  uint64_t frame_index = 0;  // the frame that fired it
  double frame_ms = 0.0;
  double baseline_ms = 0.0;  // the median it was measured against
  bool throttled = false;    // ... and whether that frame was waiting for a blank anyway
  uint32_t frames = 0;
  uint32_t events = 0;
  uint32_t samples = 0;
  // The capture hit its fixed capacity and holds only part of the window. Raise
  // Config::capture_events / capture_samples, or narrow pre/post.
  bool truncated = false;
};

void SetTrigger(const TriggerConfig &config);
const TriggerConfig &Trigger();
// The running median frame time the trigger compares against, over the last 64 frames. Exposed
// because a threshold nobody can see the baseline for is a threshold nobody can set.
double BaselineMs();
std::vector<CaptureInfo> Captures();
void ClearCaptures();

struct SiteInfo {
  const char *name = "";
  Cat cat = Cat::None;
};
// Every site registered so far - which is every site whose code has executed at least once,
// armed or not. What `prof.sites` lists, and the answer to "is this path instrumented".
std::vector<SiteInfo> SiteList();

struct SamplerStats {
  bool running = false;
  uint32_t hz = 0;      // what was asked for
  uint64_t taken = 0;
  uint64_t missed = 0;  // suspend or GetThreadContext refused
  uint64_t skipped = 0; // a tick that found no live thread to sample
  uint64_t ticks = 0;
  // Walks attempted, and ones that produced no caller at all. Barren is normally a few percent
  // (a sample taken in a prologue before EBP is set up, or in a leaf without a frame). A barren
  // rate in the tens of percent means the walk is being rejected rather than terminating, and
  // the thing to check is `stack_low`/`stack_high` - a thread whose committed stack region was
  // never resolved walks nothing at all, by design.
  uint64_t walks = 0;
  uint64_t barren = 0;
  double drift_ms = 0.0; // wall clock minus ticks x period
  // What the sampler actually achieved, and the number to read rather than `hz`. A periodic
  // waitable timer does not hold 1 kHz under this load - the first measured run asked for 1000
  // and got 566 - and a flat profile is only proportional to time if the sampling is uniform.
  double effective_hz = 0.0;
};
const SamplerStats &Sampler();

// What the profiler costs itself: the per-frame bookkeeping, timed by the same clock. Read it
// before believing anything else here.
double OverheadMs();

// Resolves an absolute address to "module+0xrva" or, once a symbol map is loaded, to a name.
// Read-time only - the sampler stores raw addresses.
std::string Describe(uint32_t address);

struct SymbolLoad {
  bool ok = false;
  uint32_t entries = 0;
  // The map's `# file_size` disagrees with the module actually loaded. It is still used - a
  // mostly-right map beats hex - but every name it produces is suspect, which is worth saying
  // out loud rather than leaving to be discovered.
  bool stale = false;
  std::string note;
};

// A symbol map for one module, as `<hex rva> <hex size> <name>` lines with `#` comments.
// gl.exe ships no symbols, but the Ghidra database is heavily named, so `utils/symdump/`
// exports one and a sampled profile reads in the game's own function names. `module` is a leaf
// file name as GetModuleFileName reports it ("gl.exe"). Read-time only.
SymbolLoad LoadSymbols(const std::string &module, const std::string &path);

// Tried once per module, the first time Describe has to name an address in it:
// `<game dir>\gkplus\symbols\<module>.sym`. A missing file is not an error and is not retried.
// This is why a sampled profile reads in names with no REPL call - drop the map in and it works.
std::string SymbolDirectory();

// A Chrome-trace / Perfetto document for `window`: zones as complete events, samples as instants
// carrying their caller chain, one track per thread, and frames on a track of their own labelled
// with whether they were throttled. Written through the ordinary CRT, not the VFS - this is a
// developer artifact, not game data.
bool WriteTrace(const std::string &path, const Window &window, std::string *error);

void Reset();

} // namespace gk::prof
