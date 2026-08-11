#include "Profiler.h"

#include "Core.h"

#include <windows.h>

#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <unordered_map>

namespace gk::prof {
namespace {

// --- storage layout -------------------------------------------------------------------------

// Eight is two game threads, the REPL, the sampler and headroom. Slots are static so a thread
// can record before anything is allocated - the ring pointer is what gates recording, not the
// slot.
constexpr uint32_t kMaxThreads = 8;
constexpr uint32_t kMaxSites = 4096;

enum : uint8_t { kScope = 0, kInstant = 1, kCounter = 2 };

// Recorded on *close*, not as a begin/end pair: half the events, and the start timestamp is
// already in the Zone object so no shadow stack is needed. Records therefore come out in end
// order - children before their parent - which every reader here accounts for by sorting on
// t_begin before it walks anything.
struct Event {
  uint64_t t_begin;
  uint32_t dur; // ticks; the value for kCounter, 0 for kInstant
  uint16_t site;
  uint8_t depth;
  uint8_t kind;
};
static_assert(sizeof(Event) == 16, "Event is the hot-path record; keep it 16 bytes");

struct Sample {
  uint64_t t;
  uint32_t address; // absolute EIP as sampled
  uint16_t site;    // the zone the thread had published, 0 for none
  uint8_t thread;   // slot index
  // How many frames this sample wrote into StackArena. Its slot there is the sample's own ring
  // index times Config::stack_depth, so nothing is stored per sample when stacks are off - the
  // arena is not even allocated.
  uint8_t frames;
};
static_assert(sizeof(Sample) == 16, "kept at 16 bytes: the stack lives in its own arena");

struct FrameRecord {
  uint64_t index;
  uint64_t t_begin;
  uint64_t t_end;
  const char *present;
  bool throttled;
  // Counted from the difference in the ring heads across the frame rather than by scanning it,
  // so the count is right even for a frame whose events have since been overwritten.
  uint32_t events;
  uint32_t samples;
};

struct SiteRecord {
  const char *name;
  Cat cat;
};

struct ThreadSlot {
  // 0 means free. Claimed with a CAS under RegistryLock; published last, so a sampler that sees
  // an id is guaranteed to see the handle and ring that go with it.
  std::atomic<uint32_t> thread_id{0};
  HANDLE handle = nullptr;
  Event *ring = nullptr;
  std::atomic<uint32_t> head{0}; // monotonic; index is head & (capacity - 1)
  // Published for the sampler, which reads them out of a suspended thread rather than asking
  // the thread itself. This is the join between the two sources.
  std::atomic<uint16_t> current_site{0};
  std::atomic<uint8_t> depth{0};
  char name[24] = {};
  // The committed stack region this thread was last seen in, from VirtualQuery. **This is what
  // makes the frame-pointer walk safe**: every read it performs is inside a range the OS has
  // already reported as committed and readable, so it cannot fault - which matters because
  // clang-cl gives us no working SEH on x86 (verified: __try/__except compiles and then does
  // not catch the access violation), and a faulting read inside a suspended-thread window would
  // take the process down with the game's threads stopped.
  //
  // Refreshed on the sampler thread OUTSIDE the suspend window, so VirtualQuery's cost is never
  // paid with a game thread stopped.
  uint32_t stack_low = 0;
  uint32_t stack_high = 0;
};

ThreadSlot Slots[kMaxThreads];
std::mutex RegistryLock;

thread_local ThreadSlot *TlsSlot = nullptr;

SiteRecord Sites[kMaxSites];
std::atomic<uint32_t> SiteCount{1}; // 0 is "no site", so Sites[0] stays empty
std::mutex SiteLock;

Config TheConfig;
std::atomic<bool> IsArmed{false};
uint32_t EventCapacity = 0; // power of two
Event *EventArena = nullptr;

FrameRecord *FrameRing = nullptr;
uint32_t FrameCapacity = 0;
uint64_t FrameCount = 0; // monotonic; index is FrameCount % FrameCapacity
uint64_t FrameIndex = 0;
uint64_t FrameOpenedAt = 0;

Sample *SampleRing = nullptr;
uint32_t SampleCapacity = 0;
std::atomic<uint32_t> SampleHead{0};
// SampleCapacity * StackDepth words, allocated only when Config::stacks is on.
uint32_t *StackArena = nullptr;
uint32_t StackDepth = 0;

// The loaded modules' address ranges, so a candidate return address can be tested against code
// without calling anything. Rebuilt on the sampler thread, never inside a suspend window:
// EnumProcessModules takes the loader lock, and taking the loader lock while a game thread is
// suspended inside it is the classic profiler deadlock.
struct CodeRange {
  uint32_t base;
  uint32_t end;
};
CodeRange CodeRanges[64];
uint32_t CodeRangeCount = 0;

void RefreshCodeRanges() {
  HMODULE modules[64];
  DWORD needed = 0;
  if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
    return;
  }
  const uint32_t count = (needed / sizeof(HMODULE)) < 64 ? needed / sizeof(HMODULE) : 64;
  uint32_t written = 0;
  for (uint32_t i = 0; i < count; ++i) {
    MODULEINFO info;
    if (!GetModuleInformation(GetCurrentProcess(), modules[i], &info, sizeof(info))) {
      continue;
    }
    CodeRanges[written].base = reinterpret_cast<uint32_t>(info.lpBaseOfDll);
    CodeRanges[written].end = CodeRanges[written].base + info.SizeOfImage;
    ++written;
  }
  CodeRangeCount = written;
}

// Whether `address` lands in a loaded image. Module extent rather than the executable sections
// specifically - which is loose, but its job is to reject a stack slot that holds a pointer or
// an integer rather than to prove the target is an instruction.
inline bool LooksLikeCode(uint32_t address) {
  for (uint32_t i = 0; i < CodeRangeCount; ++i) {
    if (address >= CodeRanges[i].base && address < CodeRanges[i].end) {
      return true;
    }
  }
  return false;
}

// Walks EBP frames into `out`, returning how many it wrote. Leaf-first: the caller stores the
// EIP itself separately, so out[0] is the EIP's caller.
//
// **Every dereference is inside [low, high)**, which the caller has proved committed and
// readable with VirtualQuery, so this cannot fault. The other three guards are correctness
// rather than safety: a frame pointer must be 4-aligned, must climb strictly (or a corrupt
// chain loops forever), and its return address must land in a loaded image.
uint8_t WalkFrames(uint32_t esp, uint32_t ebp, uint32_t low, uint32_t high, uint32_t *out,
                   uint8_t max) {
  if (high <= low || esp < low || esp >= high) {
    return 0;
  }
  uint32_t frame = ebp;
  uint32_t floor = esp;
  uint8_t written = 0;
  while (written < max) {
    if (frame < floor || frame + 8 > high || (frame & 3) != 0) {
      break;
    }
    const uint32_t next = *reinterpret_cast<const uint32_t *>(frame);
    const uint32_t ret = *reinterpret_cast<const uint32_t *>(frame + 4);
    if (!LooksLikeCode(ret)) {
      break;
    }
    out[written++] = ret;
    if (next <= frame) {
      break;
    }
    floor = frame;
    frame = next;
  }
  return written;
}

const char *PresentHow = "unknown";
bool PresentThrottled = false;

// --- the trigger's storage ------------------------------------------------------------------

// One captured window. Every vector is reserved to its capacity at Arm and only ever cleared and
// re-filled, so taking a capture is memcpy and never allocation - it happens on the main thread
// inside Present, immediately after a frame that already stuttered, and a malloc there would be
// a second stutter attributed to the first one's cause.
struct Capture {
  bool filled = false;
  uint64_t frame_index = 0;
  double frame_ms = 0.0;
  double baseline_ms = 0.0;
  bool throttled = false;
  bool truncated = false;
  uint64_t t_begin = 0;
  uint64_t t_end = 0;
  std::vector<FrameRecord> frames;
  std::vector<Event> events[kMaxThreads];
  std::vector<Sample> samples;
  std::vector<uint32_t> stacks; // samples.size() * StackDepth, when stacks are on
};

std::vector<Capture> CaptureRing;
uint32_t CaptureNext = 0;  // where the next capture goes; the ring drops the oldest
uint32_t CaptureCount = 0; // how many are filled, up to CaptureRing.size()
TriggerConfig TheTrigger;

// The last 64 frame times, for the trigger's median. A median and not a mean: a mean is dragged
// up by the very frames it is supposed to make stand out, so after a few stutters it stops
// firing on them.
double RecentMs[64] = {};
uint32_t RecentCount = 0;
uint32_t RecentNext = 0;
double Baseline = 0.0;
// Set when a frame fires the trigger; the snapshot is taken this many frames later so the
// capture contains the recovery as well as the stutter.
bool CapturePending = false;
uint64_t CaptureDueAt = 0;
uint64_t CaptureTriggerFrame = 0;
double CaptureTriggerMs = 0.0;
double CaptureTriggerBaseline = 0.0;
bool CaptureTriggerThrottled = false;

SamplerStats TheSamplerStats;
HANDLE SamplerThread = nullptr;
HANDLE SamplerStop = nullptr;
// Bumped by Reset; the sampler restarts its tick count and its clock when it notices.
std::atomic<uint32_t> SamplerRebase{0};

double TicksToMs = 0.0;
uint64_t ZoneCostTicks = 0; // calibrated at Arm; see OverheadMs
uint64_t FrameMarkTicks = 0;

// --- clock ----------------------------------------------------------------------------------

// QPC rather than rdtsc: on anything this runs on it is already rdtsc-backed through the
// user-mode shared data page, so it costs the same and is invariant across cores, which raw
// rdtsc on a 2000-era-targeted 32-bit process is not guaranteed to be.
inline uint64_t Now() {
  LARGE_INTEGER v;
  QueryPerformanceCounter(&v);
  return static_cast<uint64_t>(v.QuadPart);
}

double InitTicksToMs() {
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  return f.QuadPart != 0 ? 1000.0 / static_cast<double>(f.QuadPart) : 0.0;
}

// --- thread slots ---------------------------------------------------------------------------

bool ThreadStillAlive(HANDLE h) {
  if (h == nullptr) {
    return false;
  }
  DWORD code = 0;
  return GetExitCodeThread(h, &code) && code == STILL_ACTIVE;
}

void NameSlot(ThreadSlot *slot, uint32_t id) {
  unsigned long *executing_thread = nullptr;
  GetObjectAtOffset(executing_thread, 0x007b9d7c);
  // Zero until StartExecutorThread runs, so before a level loads every thread is "some thread"
  // and the main one is named by being the one that marks frames.
  if (executing_thread != nullptr && *executing_thread == id) {
    std::snprintf(slot->name, sizeof(slot->name), "executor");
  } else {
    std::snprintf(slot->name, sizeof(slot->name), "thread-%u", id);
  }
}

// The slow path, once per thread. Reclaims a slot whose thread has exited, which matters because
// the executor is created at level start and destroyed at level end - without reclamation eight
// level loads would exhaust the table.
ThreadSlot *ClaimSlot() {
  const uint32_t id = GetCurrentThreadId();
  std::lock_guard<std::mutex> guard(RegistryLock);

  ThreadSlot *free_slot = nullptr;
  for (ThreadSlot &slot : Slots) {
    const uint32_t owner = slot.thread_id.load(std::memory_order_relaxed);
    if (owner == id) {
      TlsSlot = &slot;
      return &slot;
    }
    if (free_slot == nullptr &&
        (owner == 0 || (slot.handle != nullptr && !ThreadStillAlive(slot.handle)))) {
      free_slot = &slot;
    }
  }
  if (free_slot == nullptr) {
    return nullptr;
  }

  // A reclaimed slot's handle is closed here rather than at thread exit, and only while the
  // registry lock is held - which is the same lock the sampler holds for the whole of a tick,
  // so it cannot be closed out from under a suspend/resume pair.
  if (free_slot->handle != nullptr) {
    CloseHandle(free_slot->handle);
    free_slot->handle = nullptr;
  }
  free_slot->head.store(0, std::memory_order_relaxed);
  free_slot->depth.store(0, std::memory_order_relaxed);
  free_slot->current_site.store(0, std::memory_order_relaxed);
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                  &free_slot->handle, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);
  NameSlot(free_slot, id);
  if (EventArena != nullptr) {
    free_slot->ring =
        EventArena + static_cast<size_t>(free_slot - Slots) * EventCapacity;
  }
  free_slot->thread_id.store(id, std::memory_order_release);
  TlsSlot = free_slot;
  return free_slot;
}

// A thread becomes visible to the profiler by *recording* something - there is deliberately no
// discovery pass that registers a thread the profiler has never been called from. One was
// written, against the game's own `ExecutingThread` global, and removed for two independent
// reasons, both measured:
//
//   * **It found the main thread.** `*(DWORD *)0x007b9d7c` read the id of the thread that
//     presents, not an executor, through a whole level02 session - so it could never have
//     registered the thread it was for.
//   * **Its handle broke the sampler.** A slot whose handle comes from `OpenThread` makes
//     `GetThreadContext` return the stale WOW64 context - every sample read
//     `ntdll!NtWaitForMultipleObjects+0xc`, the 32->64 transition, whatever the thread was
//     actually running - where the same thread sampled through a `DuplicateHandle`-derived
//     handle gives live EIPs. Reproduced four times, and A/B'd on one build. Both handles are
//     opened with THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, so the access mask is not the
//     difference; the cause is unexplained and the workaround is to not do it.
//
// The cost is that a thread which never runs instrumented code is never profiled. That is worth
// knowing about rather than papering over: in every measured level02 session so far the
// executor did not reach a single zone, and this is the fact that says so.

inline ThreadSlot *CurrentSlot() {
  ThreadSlot *slot = TlsSlot;
  return slot != nullptr ? slot : ClaimSlot();
}

inline void Push(ThreadSlot *slot, const Event &event) {
  Event *ring = slot->ring;
  if (ring == nullptr) {
    return;
  }
  const uint32_t head = slot->head.load(std::memory_order_relaxed);
  ring[head & (EventCapacity - 1)] = event;
  slot->head.store(head + 1, std::memory_order_release);
}

const char *SiteName(uint16_t site) {
  return site != 0 && site < SiteCount.load(std::memory_order_acquire) ? Sites[site].name : "";
}

// --- the sampler ----------------------------------------------------------------------------

DWORD WINAPI SamplerProc(LPVOID) {
  const uint32_t self = GetCurrentThreadId();
  const uint32_t hz = TheConfig.sampler_hz != 0 ? TheConfig.sampler_hz : 1000;
  const uint64_t period_100ns = 10000000ull / hz;

  // A high-resolution waitable timer, not timeBeginPeriod: raising the system timer resolution
  // would change the scheduling of the game's own loop, which is the thing being measured.
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  if (timer != nullptr) {
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(period_100ns);
    if (!SetWaitableTimer(timer, &due, static_cast<LONG>(1000 / hz != 0 ? 1000 / hz : 1),
                          nullptr, nullptr, FALSE)) {
      CloseHandle(timer);
      timer = nullptr;
    }
  }

  uint64_t started = Now();
  uint64_t ticks = 0;
  uint32_t rebase_seen = SamplerRebase.load(std::memory_order_relaxed);
  RefreshCodeRanges();

  for (;;) {
    HANDLE waits[2] = {SamplerStop, timer};
    const DWORD count = timer != nullptr ? 2u : 1u;
    const DWORD reason =
        WaitForMultipleObjects(count, waits, FALSE, timer != nullptr ? INFINITE : 1);
    if (reason == WAIT_OBJECT_0) {
      break;
    }
    // Rebased by Reset, so `taken` and `ticks` stay comparable - resetting one and not the other
    // made the first measured run look like it had missed 80% of its samples when it had not.
    if (SamplerRebase.load(std::memory_order_relaxed) != rebase_seen) {
      rebase_seen = SamplerRebase.load(std::memory_order_relaxed);
      started = Now();
      ticks = 0;
    }
    ++ticks;
    TheSamplerStats.ticks = ticks;
    // Before the registry lock and outside every suspend window, for the loader-lock reason
    // given at RefreshCodeRanges. Periodic rather than once: the game loads DLLs as it runs.
    if ((ticks & 2047) == 1) {
      RefreshCodeRanges();
    }
    // Live, not at shutdown. Computed there it read 0 for the whole session, which is the least
    // useful possible value for the number that says whether the sample rate is what it claims.
    const double elapsed_ms = static_cast<double>(Now() - started) * TicksToMs;
    TheSamplerStats.drift_ms =
        elapsed_ms - static_cast<double>(ticks) * (1000.0 / static_cast<double>(hz));
    TheSamplerStats.effective_hz =
        elapsed_ms > 0.0 ? static_cast<double>(ticks) * 1000.0 / elapsed_ms : 0.0;

    bool sampled_any = false;
    {
      // Held across the whole tick. A game thread blocked here is a game thread that is not
      // inside ClaimSlot, which is what makes a slot's handle safe to suspend through; and the
      // sampler never waits on a thread it has suspended, so there is no cycle.
      std::lock_guard<std::mutex> guard(RegistryLock);
      for (uint32_t i = 0; i < kMaxThreads; ++i) {
        ThreadSlot &slot = Slots[i];
        const uint32_t id = slot.thread_id.load(std::memory_order_acquire);
        if (id == 0 || id == self || slot.handle == nullptr) {
          continue;
        }
        if (SuspendThread(slot.handle) == static_cast<DWORD>(-1)) {
          ++TheSamplerStats.missed;
          continue;
        }
        CONTEXT context;
        std::memset(&context, 0, sizeof(context));
        context.ContextFlags = CONTEXT_CONTROL;
        // SuspendThread is asynchronous; GetThreadContext is what actually blocks until the
        // suspend has landed, so the EIP below is only trustworthy because of this call.
        const BOOL ok = GetThreadContext(slot.handle, &context);
        const uint16_t site = slot.current_site.load(std::memory_order_relaxed);
        // The walk has to happen here, with the thread still stopped - the frames it reads are
        // live stack. It performs no call and no allocation, and cannot fault, because
        // slot.stack_low/high were proved committed by a VirtualQuery done outside this window.
        uint32_t frames[kMaxStackFrames];
        uint8_t frame_count = 0;
        if (ok && StackArena != nullptr) {
          frame_count = WalkFrames(context.Esp, context.Ebp, slot.stack_low, slot.stack_high,
                                   frames, static_cast<uint8_t>(StackDepth - 1));
        }
        ResumeThread(slot.handle);
        // Nothing between the suspend and the resume above allocates, formats or takes a lock
        // the suspended thread could hold. Recording happens after it is running again.
        if (!ok) {
          ++TheSamplerStats.missed;
          continue;
        }
        // Outside the suspend window on purpose: this is the query that makes the walk above
        // safe on the NEXT sample, and it is a syscall.
        if (context.Esp < slot.stack_low || context.Esp >= slot.stack_high) {
          MEMORY_BASIC_INFORMATION mbi;
          if (VirtualQuery(reinterpret_cast<LPCVOID>(context.Esp), &mbi, sizeof(mbi)) != 0 &&
              mbi.State == MEM_COMMIT &&
              (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                              PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) != 0) {
            slot.stack_low = reinterpret_cast<uint32_t>(mbi.BaseAddress);
            slot.stack_high = slot.stack_low + static_cast<uint32_t>(mbi.RegionSize);
          } else {
            slot.stack_low = 0;
            slot.stack_high = 0;
          }
        }
        if (SampleRing != nullptr) {
          const uint32_t head = SampleHead.load(std::memory_order_relaxed);
          const uint32_t index = head & (SampleCapacity - 1);
          Sample &out = SampleRing[index];
          out.t = Now();
          out.address = context.Eip;
          out.site = site;
          out.thread = static_cast<uint8_t>(i);
          out.frames = 0;
          if (StackArena != nullptr) {
            uint32_t *slots = StackArena + static_cast<size_t>(index) * StackDepth;
            slots[0] = context.Eip;
            for (uint8_t f = 0; f < frame_count; ++f) {
              slots[f + 1] = frames[f];
            }
            out.frames = static_cast<uint8_t>(frame_count + 1);
            ++TheSamplerStats.walks;
            if (frame_count == 0) {
              ++TheSamplerStats.barren;
            }
          }
          SampleHead.store(head + 1, std::memory_order_release);
          ++TheSamplerStats.taken;
          sampled_any = true;
        }
      }
    }
    if (!sampled_any) {
      ++TheSamplerStats.skipped;
    }
  }

  if (timer != nullptr) {
    CancelWaitableTimer(timer);
    CloseHandle(timer);
  }
  return 0;
}

void StartSampler() {
  if (SamplerThread != nullptr || !TheConfig.sampler) {
    return;
  }
  SamplerStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (SamplerStop == nullptr) {
    return;
  }
  SamplerThread = CreateThread(nullptr, 0, SamplerProc, nullptr, 0, nullptr);
  if (SamplerThread == nullptr) {
    CloseHandle(SamplerStop);
    SamplerStop = nullptr;
    return;
  }
  // Above both game threads (the executor runs at THREAD_PRIORITY_HIGHEST) so a tick is not
  // itself delayed by the load it is measuring.
  SetThreadPriority(SamplerThread, THREAD_PRIORITY_TIME_CRITICAL);
  TheSamplerStats.running = true;
  TheSamplerStats.hz = TheConfig.sampler_hz;
}

void StopSampler() {
  if (SamplerThread == nullptr) {
    return;
  }
  SetEvent(SamplerStop);
  WaitForSingleObject(SamplerThread, 2000);
  CloseHandle(SamplerThread);
  CloseHandle(SamplerStop);
  SamplerThread = nullptr;
  SamplerStop = nullptr;
  TheSamplerStats.running = false;
}

// --- read-side helpers ------------------------------------------------------------------------

// A copy of one thread's events whose t_begin falls in [from, to).
struct Slice {
  uint32_t thread = 0;
  std::vector<Event> events;
};

void SortSlice(Slice &slice) {
  std::sort(slice.events.begin(), slice.events.end(),
            [](const Event &a, const Event &b) {
              if (a.t_begin != b.t_begin) {
                return a.t_begin < b.t_begin;
              }
              // A tie has to put the ENCLOSING zone first, or the stack walk in Zones()
              // adopts the parent as its own child's child and subtracts the parent's whole
              // duration from the child - which underflows, both being unsigned. Not a corner
              // case: QPC ticks at 100 ns here, which is a few hundred instructions, and an
              // outer zone whose first statement opens an inner one lands on the same tick
              // routinely. It read as 645636042579833 ms/frame for upload/ConvertVertices the
              // first time this ran.
              return a.t_begin + a.dur > b.t_begin + b.dur;
            });
}

std::vector<Slice> SliceEvents(const Window &window) {
  std::vector<Slice> out;
  if (!window.valid) {
    return out;
  }
  // A capture is already a copy of exactly this window, so it needs slicing only to keep the
  // per-thread shape and the sort the walk below depends on.
  if (window.capture >= 0) {
    if (static_cast<size_t>(window.capture) >= CaptureRing.size()) {
      return out;
    }
    const Capture &capture = CaptureRing[window.capture];
    for (uint32_t i = 0; i < kMaxThreads; ++i) {
      if (capture.events[i].empty()) {
        continue;
      }
      Slice slice;
      slice.thread = i;
      slice.events = capture.events[i];
      SortSlice(slice);
      out.push_back(std::move(slice));
    }
    return out;
  }
  const uint64_t from = window.t_begin;
  const uint64_t to = window.t_end;
  if (EventArena == nullptr) {
    return out;
  }
  for (uint32_t i = 0; i < kMaxThreads; ++i) {
    ThreadSlot &slot = Slots[i];
    if (slot.ring == nullptr || slot.thread_id.load(std::memory_order_acquire) == 0) {
      continue;
    }
    const uint32_t head = slot.head.load(std::memory_order_acquire);
    const uint32_t available = head < EventCapacity ? head : EventCapacity;
    Slice slice;
    slice.thread = i;
    for (uint32_t n = 0; n < available; ++n) {
      const Event event = slot.ring[(head - available + n) & (EventCapacity - 1)];
      if (event.t_begin >= from && event.t_begin < to) {
        slice.events.push_back(event);
      }
    }
    // The writer may have lapped us mid-copy. Rather than report a torn window, drop anything
    // the writer has since overwritten by re-reading the head and trimming.
    const uint32_t head_after = slot.head.load(std::memory_order_acquire);
    if (head_after - head >= EventCapacity) {
      slice.events.clear();
    }
    if (!slice.events.empty()) {
      SortSlice(slice);
      out.push_back(std::move(slice));
    }
  }
  return out;
}

// The oldest frame index still held, and how many there are.
bool HeldFrames(uint64_t *oldest, uint32_t *count) {
  if (FrameRing == nullptr || FrameCount == 0) {
    return false;
  }
  const uint32_t have =
      static_cast<uint32_t>(FrameCount < FrameCapacity ? FrameCount : FrameCapacity);
  *oldest = FrameCount - have;
  *count = have;
  return true;
}

// A window over ring positions [first, last], where those are counts-since-start rather than
// the frames' own `index` fields. The two differ after a Reset, which restarts the count but
// not the index - so everything that takes a *frame index* converts through here.
Window WindowOverPositions(uint64_t first, uint64_t last) {
  Window window;
  uint64_t oldest = 0;
  uint32_t have = 0;
  if (!HeldFrames(&oldest, &have)) {
    return window;
  }
  if (first < oldest) {
    first = oldest;
  }
  if (last >= FrameCount) {
    last = FrameCount - 1;
  }
  if (first > last) {
    return window;
  }
  const FrameRecord &begin = FrameRing[first % FrameCapacity];
  const FrameRecord &end = FrameRing[last % FrameCapacity];
  window.t_begin = begin.t_begin;
  window.t_end = end.t_end;
  window.first_index = begin.index;
  window.last_index = end.index;
  window.frames = static_cast<uint32_t>(last - first + 1);
  window.valid = window.t_end > window.t_begin;
  return window;
}

FrameSummary Summarize(const FrameRecord &record) {
  FrameSummary out;
  out.index = record.index;
  out.ms = static_cast<double>(record.t_end - record.t_begin) * TicksToMs;
  out.throttled = record.throttled;
  out.present = record.present;
  out.events = record.events;
  out.samples = record.samples;
  out.t_begin = record.t_begin;
  return out;
}

// --- symbols ----------------------------------------------------------------------------------

struct SymbolEntry {
  uint32_t rva;
  uint32_t size;
  std::string name;
};

struct SymbolTable {
  std::vector<SymbolEntry> entries; // sorted by rva
};

std::unordered_map<std::string, SymbolTable> SymbolTables;
// Modules Describe has already tried to auto-load a map for. A miss is recorded here so the
// filesystem is touched once per module rather than once per address.
std::unordered_map<std::string, bool> SymbolAutoloadTried;

// The size on disk of a loaded module, for the staleness check. 0 when it cannot be read.
uint64_t ModuleFileSize(const std::string &module) {
  HMODULE handle = GetModuleHandleA(module.c_str());
  if (handle == nullptr) {
    return 0;
  }
  char path[MAX_PATH] = {};
  if (GetModuleFileNameA(handle, path, MAX_PATH) == 0) {
    return 0;
  }
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) {
    return 0;
  }
  return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

struct ModuleRange {
  uintptr_t base;
  std::string name;
};
// A deque, not a vector: ModuleFor hands back a pointer into it and a later miss must not move
// what an earlier one returned.
std::deque<ModuleRange> ModuleCache;

const ModuleRange *ModuleFor(uint32_t address) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(address)),
                          &module)) {
    return nullptr;
  }
  const uintptr_t base = reinterpret_cast<uintptr_t>(module);
  for (const ModuleRange &range : ModuleCache) {
    if (range.base == base) {
      return &range;
    }
  }
  char path[MAX_PATH] = {};
  GetModuleFileNameA(module, path, MAX_PATH);
  const char *leaf = std::strrchr(path, '\\');
  ModuleCache.push_back({base, leaf != nullptr ? leaf + 1 : path});
  return &ModuleCache.back();
}

} // namespace

// --- public: recording ------------------------------------------------------------------------

std::atomic<uint32_t> ActiveMask{0};

uint16_t RegisterSite(const char *name, Cat cat) {
  std::lock_guard<std::mutex> guard(SiteLock);
  const uint32_t next = SiteCount.load(std::memory_order_relaxed);
  if (next >= kMaxSites) {
    return 0;
  }
  Sites[next].name = name;
  Sites[next].cat = cat;
  SiteCount.store(next + 1, std::memory_order_release);
  return static_cast<uint16_t>(next);
}

uint64_t Open(uint16_t site) {
  ThreadSlot *slot = CurrentSlot();
  if (slot == nullptr || slot->ring == nullptr) {
    return 0;
  }
  slot->depth.fetch_add(1, std::memory_order_relaxed);
  slot->current_site.store(site, std::memory_order_relaxed);
  return Now();
}

void Close(uint16_t site, uint64_t begin) {
  ThreadSlot *slot = TlsSlot;
  if (slot == nullptr) {
    return;
  }
  const uint64_t end = Now();
  uint8_t depth = slot->depth.load(std::memory_order_relaxed);
  if (depth > 0) {
    --depth;
    slot->depth.store(depth, std::memory_order_relaxed);
  }
  // The enclosing zone's site is not tracked - publishing 0 on close is what makes a sample
  // taken between two sibling zones read as "no zone" rather than as the previous one.
  slot->current_site.store(0, std::memory_order_relaxed);
  Event event;
  event.t_begin = begin;
  event.dur = static_cast<uint32_t>(end - begin);
  event.site = site;
  event.depth = depth;
  event.kind = kScope;
  Push(slot, event);
}

void Instant(uint16_t site) {
  ThreadSlot *slot = CurrentSlot();
  if (slot == nullptr || slot->ring == nullptr) {
    return;
  }
  Event event;
  event.t_begin = Now();
  event.dur = 0;
  event.site = site;
  event.depth = slot->depth.load(std::memory_order_relaxed);
  event.kind = kInstant;
  Push(slot, event);
}

void Counter(uint16_t site, uint32_t value) {
  ThreadSlot *slot = CurrentSlot();
  if (slot == nullptr || slot->ring == nullptr) {
    return;
  }
  Event event;
  event.t_begin = Now();
  event.dur = value;
  event.site = site;
  event.depth = slot->depth.load(std::memory_order_relaxed);
  event.kind = kCounter;
  Push(slot, event);
}

namespace {

// The median of the last 64 frame times. A median rather than a mean because a mean is dragged
// up by the very frames the trigger exists to notice, so a run with a few stutters in it stops
// firing on the next one.
double UpdateBaseline(double ms) {
  RecentMs[RecentNext] = ms;
  RecentNext = (RecentNext + 1) % 64;
  if (RecentCount < 64) {
    ++RecentCount;
  }
  double sorted[64];
  std::memcpy(sorted, RecentMs, sizeof(double) * RecentCount);
  std::nth_element(sorted, sorted + RecentCount / 2, sorted + RecentCount);
  Baseline = sorted[RecentCount / 2];
  return Baseline;
}

// Copies the window around the frame that fired the trigger out of the live rings. Main thread,
// inside Present, and allocation-free: every destination was reserved at Arm and is only ever
// cleared and re-filled.
void TakeCapture() {
  if (CaptureRing.empty()) {
    return;
  }
  const Window window = AroundFrame(CaptureTriggerFrame, TheTrigger.pre, TheTrigger.post);
  if (!window.valid) {
    return;
  }
  Capture &capture = CaptureRing[CaptureNext];
  CaptureNext = (CaptureNext + 1) % static_cast<uint32_t>(CaptureRing.size());
  if (CaptureCount < CaptureRing.size()) {
    ++CaptureCount;
  }

  capture.filled = true;
  capture.truncated = false;
  capture.frame_index = CaptureTriggerFrame;
  capture.frame_ms = CaptureTriggerMs;
  capture.baseline_ms = CaptureTriggerBaseline;
  capture.throttled = CaptureTriggerThrottled;
  capture.t_begin = window.t_begin;
  capture.t_end = window.t_end;

  capture.frames.clear();
  uint64_t oldest = 0;
  uint32_t have = 0;
  if (HeldFrames(&oldest, &have)) {
    for (uint64_t position = oldest; position < FrameCount; ++position) {
      const FrameRecord &record = FrameRing[position % FrameCapacity];
      if (record.index >= window.first_index && record.index <= window.last_index &&
          capture.frames.size() < capture.frames.capacity()) {
        capture.frames.push_back(record);
      }
    }
  }

  for (uint32_t i = 0; i < kMaxThreads; ++i) {
    capture.events[i].clear();
    ThreadSlot &slot = Slots[i];
    if (slot.ring == nullptr || slot.thread_id.load(std::memory_order_acquire) == 0) {
      continue;
    }
    const uint32_t head = slot.head.load(std::memory_order_acquire);
    const uint32_t available = head < EventCapacity ? head : EventCapacity;
    for (uint32_t n = 0; n < available; ++n) {
      const Event event = slot.ring[(head - available + n) & (EventCapacity - 1)];
      if (event.t_begin < window.t_begin || event.t_begin >= window.t_end) {
        continue;
      }
      if (capture.events[i].size() >= capture.events[i].capacity()) {
        capture.truncated = true;
        break;
      }
      capture.events[i].push_back(event);
    }
  }

  capture.samples.clear();
  capture.stacks.clear();
  if (SampleRing != nullptr) {
    const uint32_t head = SampleHead.load(std::memory_order_acquire);
    const uint32_t available = head < SampleCapacity ? head : SampleCapacity;
    for (uint32_t n = 0; n < available; ++n) {
      const uint32_t index = (head - available + n) & (SampleCapacity - 1);
      const Sample sample = SampleRing[index];
      if (sample.t < window.t_begin || sample.t >= window.t_end) {
        continue;
      }
      if (capture.samples.size() >= capture.samples.capacity()) {
        capture.truncated = true;
        break;
      }
      capture.samples.push_back(sample);
      if (StackArena != nullptr) {
        const uint32_t *slots = StackArena + static_cast<size_t>(index) * StackDepth;
        capture.stacks.insert(capture.stacks.end(), slots, slots + StackDepth);
      }
    }
  }
}

// Called once per frame from FrameMark, with the frame that has just closed.
void ServiceTrigger(const FrameRecord &closed) {
  const double ms = static_cast<double>(closed.t_end - closed.t_begin) * TicksToMs;
  const double baseline = UpdateBaseline(ms);
  if (!TheTrigger.enabled || CaptureRing.empty()) {
    return;
  }
  // The deferred snapshot comes first: a second stutter inside the post window must not restart
  // the countdown, or a run of them never captures anything at all.
  if (CapturePending) {
    if (FrameCount >= CaptureDueAt) {
      TakeCapture();
      CapturePending = false;
    }
    return;
  }
  // A baseline needs enough frames to mean anything - a level load is the first thing the
  // profiler ever sees, and it would otherwise fire the trigger against a median of one.
  if (RecentCount < 16) {
    return;
  }
  if (ms < TheTrigger.min_ms || ms < baseline * TheTrigger.multiple) {
    return;
  }
  CapturePending = true;
  CaptureDueAt = FrameCount + TheTrigger.post;
  CaptureTriggerFrame = closed.index;
  CaptureTriggerMs = ms;
  CaptureTriggerBaseline = baseline;
  CaptureTriggerThrottled = closed.throttled;
}

} // namespace

// --- public: frames ---------------------------------------------------------------------------

void FrameMark() {
  // The one-shot boot check. Here rather than in DllMain because Arm allocates several
  // megabytes and starts a thread, neither of which belongs under the loader lock - and this is
  // the earliest point at which the game is unambiguously running.
  //
  //   GKPLUS_PROFILER=1        zones and sampler
  //   GKPLUS_PROFILER=zones    zones only
  //   GKPLUS_PROFILER=stacks   zones, sampler, and frame-pointer walks
  //   GKPLUS_PROFILER_HZ=250   sampler rate
  static bool considered = false;
  if (!considered) {
    considered = true;
    char value[32] = {};
    if (GetEnvironmentVariableA("GKPLUS_PROFILER", value, sizeof(value)) != 0 &&
        value[0] != '0') {
      Config config;
      config.sampler = std::strcmp(value, "zones") != 0;
      config.stacks = std::strcmp(value, "stacks") == 0;
      char hz[16] = {};
      if (GetEnvironmentVariableA("GKPLUS_PROFILER_HZ", hz, sizeof(hz)) != 0) {
        const int parsed = std::atoi(hz);
        if (parsed > 0 && parsed <= 8000) {
          config.sampler_hz = static_cast<uint32_t>(parsed);
        }
      }
      Arm(config);
    }
  }

  if (!IsArmed.load(std::memory_order_relaxed) || FrameRing == nullptr) {
    return;
  }
  const uint64_t now = Now();

  // Summing the heads rather than scanning the ring: a frame's event count then survives its
  // events being overwritten, which is what makes OverheadMs readable on an old frame.
  uint32_t heads = 0;
  for (const ThreadSlot &slot : Slots) {
    heads += slot.head.load(std::memory_order_acquire);
  }
  const uint32_t sample_head = SampleHead.load(std::memory_order_acquire);
  static uint32_t last_heads = 0;
  static uint32_t last_sample_head = 0;

  if (FrameOpenedAt != 0) {
    FrameRecord &record = FrameRing[FrameCount % FrameCapacity];
    record.index = FrameIndex++;
    record.t_begin = FrameOpenedAt;
    record.t_end = now;
    record.present = PresentHow;
    record.throttled = PresentThrottled;
    record.events = heads - last_heads;
    record.samples = sample_head - last_sample_head;
    ++FrameCount;
    // After ++FrameCount, so the deferred snapshot's countdown counts frames that exist.
    ServiceTrigger(record);
  }
  last_heads = heads;
  last_sample_head = sample_head;
  FrameOpenedAt = now;
  // The mark's own cost, by the same clock as everything else it will be compared against.
  FrameMarkTicks = Now() - now;
  // This slot is the main thread by definition - it is the one that presents.
  ThreadSlot *slot = CurrentSlot();
  if (slot != nullptr && std::strcmp(slot->name, "main") != 0) {
    std::snprintf(slot->name, sizeof(slot->name), "main");
  }
}

void SetTrigger(const TriggerConfig &config) {
  TheTrigger = config;
  // A pending countdown belongs to the old settings; a changed `post` would make it fire at the
  // wrong distance from a stutter nobody is watching for any more.
  CapturePending = false;
}

const TriggerConfig &Trigger() { return TheTrigger; }

double BaselineMs() { return Baseline; }

std::vector<CaptureInfo> Captures() {
  std::vector<CaptureInfo> out;
  for (uint32_t i = 0; i < CaptureRing.size(); ++i) {
    const Capture &capture = CaptureRing[i];
    if (!capture.filled) {
      continue;
    }
    CaptureInfo info;
    info.index = i;
    info.frame_index = capture.frame_index;
    info.frame_ms = capture.frame_ms;
    info.baseline_ms = capture.baseline_ms;
    info.throttled = capture.throttled;
    info.frames = static_cast<uint32_t>(capture.frames.size());
    for (const std::vector<Event> &events : capture.events) {
      info.events += static_cast<uint32_t>(events.size());
    }
    info.samples = static_cast<uint32_t>(capture.samples.size());
    info.truncated = capture.truncated;
    out.push_back(info);
  }
  std::sort(out.begin(), out.end(), [](const CaptureInfo &a, const CaptureInfo &b) {
    return a.frame_index < b.frame_index;
  });
  return out;
}

void ClearCaptures() {
  for (Capture &capture : CaptureRing) {
    capture.filled = false;
  }
  CaptureNext = 0;
  CaptureCount = 0;
  CapturePending = false;
}

void NotePresentMode(const char *how, bool throttled) {
  PresentHow = how != nullptr ? how : "unknown";
  PresentThrottled = throttled;
}

// --- public: arming ---------------------------------------------------------------------------

namespace {

uint32_t RoundUpPow2(uint32_t v) {
  uint32_t n = 1;
  while (n < v && n < (1u << 30)) {
    n <<= 1;
  }
  return n;
}

// Times an open/close pair so OverheadMs can price a frame's instrumentation rather than guess
// at it. Runs before the mask is published, and rewinds the ring it dirtied.
void CalibrateZoneCost() {
  ThreadSlot *slot = CurrentSlot();
  if (slot == nullptr || slot->ring == nullptr) {
    return;
  }
  static const uint16_t site = RegisterSite("prof/calibrate", Cat::Frame);
  const uint32_t head_before = slot->head.load(std::memory_order_relaxed);
  constexpr uint32_t kIterations = 4096;
  const uint64_t started = Now();
  for (uint32_t i = 0; i < kIterations; ++i) {
    Close(site, Open(site));
  }
  const uint64_t elapsed = Now() - started;
  slot->head.store(head_before, std::memory_order_relaxed);
  slot->depth.store(0, std::memory_order_relaxed);
  ZoneCostTicks = elapsed / kIterations;
}

} // namespace

bool Arm(const Config &config) {
  if (TicksToMs == 0.0) {
    TicksToMs = InitTicksToMs();
  }
  TheConfig = config;

  const uint32_t events = RoundUpPow2(config.events_per_thread < 1024 ? 1024
                                                                     : config.events_per_thread);
  const uint32_t samples = RoundUpPow2(config.samples < 1024 ? 1024 : config.samples);
  const uint32_t frames = config.frames < 8 ? 8 : config.frames;

  // The rings are allocated once and grow only. Freeing one is unsafe at any moment a recorder
  // could be inside it on the other thread, and there is no cheap way to know that it is not -
  // so a disarm publishes a zero mask and leaves the memory alone.
  //
  // Scoped, because everything after it - the calibration, which claims this thread's slot, and
  // the sampler, whose every tick takes this lock - would deadlock against it.
  {
    std::lock_guard<std::mutex> guard(RegistryLock);
    if (events > EventCapacity) {
      Event *arena = new (std::nothrow) Event[static_cast<size_t>(events) * kMaxThreads];
      if (arena == nullptr) {
        return false;
      }
      std::memset(arena, 0, sizeof(Event) * static_cast<size_t>(events) * kMaxThreads);
      EventArena = arena;
      EventCapacity = events;
      for (uint32_t i = 0; i < kMaxThreads; ++i) {
        Slots[i].head.store(0, std::memory_order_relaxed);
        Slots[i].ring = EventArena + static_cast<size_t>(i) * EventCapacity;
      }
    }
    if (samples > SampleCapacity) {
      Sample *ring = new (std::nothrow) Sample[samples];
      if (ring == nullptr) {
        return false;
      }
      std::memset(ring, 0, sizeof(Sample) * samples);
      SampleRing = ring;
      SampleCapacity = samples;
      SampleHead.store(0, std::memory_order_relaxed);
      // A larger sample ring orphans the stack arena, which is indexed by the ring index.
      StackArena = nullptr;
      StackDepth = 0;
    }
    const uint32_t depth = config.stacks ? (config.stack_depth < 2         ? 2
                                            : config.stack_depth > kMaxStackFrames
                                                ? kMaxStackFrames
                                                : config.stack_depth)
                                         : 0;
    if (depth > StackDepth) {
      uint32_t *arena =
          new (std::nothrow) uint32_t[static_cast<size_t>(SampleCapacity) * depth];
      if (arena == nullptr) {
        return false;
      }
      std::memset(arena, 0, sizeof(uint32_t) * static_cast<size_t>(SampleCapacity) * depth);
      // Published after the depth it is sized for, and the sampler tests the pointer - so a
      // tick landing mid-Arm either sees no arena or sees a complete one.
      StackDepth = depth;
      StackArena = arena;
    } else if (depth == 0) {
      StackArena = nullptr;
    }
    if (frames > FrameCapacity) {
      FrameRecord *ring = new (std::nothrow) FrameRecord[frames];
      if (ring == nullptr) {
        return false;
      }
      std::memset(ring, 0, sizeof(FrameRecord) * frames);
      FrameRing = ring;
      FrameCapacity = frames;
      FrameCount = 0;
      FrameOpenedAt = 0;
    }

    // Reserved here, never at capture time: a capture is taken on the main thread inside
    // Present, immediately after a frame that already stuttered, and a 512 KB malloc there
    // would be a second stutter that looks like part of the first one's cause.
    if (config.captures != CaptureRing.size() ||
        (!CaptureRing.empty() && CaptureRing[0].events[0].capacity() < config.capture_events)) {
      CaptureRing.clear();
      CaptureRing.resize(config.captures);
      for (Capture &capture : CaptureRing) {
        capture.frames.reserve(FrameCapacity < 4096 ? FrameCapacity : 4096);
        for (std::vector<Event> &events : capture.events) {
          events.reserve(config.capture_events);
        }
        capture.samples.reserve(config.capture_samples);
        if (depth != 0) {
          capture.stacks.reserve(static_cast<size_t>(config.capture_samples) * depth);
        }
      }
      CaptureNext = 0;
      CaptureCount = 0;
      CapturePending = false;
    }
  }

  IsArmed.store(true, std::memory_order_relaxed);
  CalibrateZoneCost();
  ActiveMask.store(config.mask, std::memory_order_relaxed);
  // A running sampler keeps the rate it was started with - StartSampler is a no-op once the
  // thread exists - so a re-arm that changes it has to stop the old one first. Without this,
  // `prof.configure({sampler_hz: 900})` reported 900 and sampled at 1000.
  if (SamplerThread != nullptr &&
      (!config.sampler || TheSamplerStats.hz != config.sampler_hz)) {
    StopSampler();
  }
  StartSampler();
  return true;
}

void Disarm() {
  ActiveMask.store(0, std::memory_order_relaxed);
  IsArmed.store(false, std::memory_order_relaxed);
  StopSampler();
}

bool Armed() { return IsArmed.load(std::memory_order_relaxed); }

void SetMask(uint32_t mask) {
  TheConfig.mask = mask;
  if (IsArmed.load(std::memory_order_relaxed)) {
    ActiveMask.store(mask, std::memory_order_relaxed);
  }
}

const Config &CurrentConfig() { return TheConfig; }

void Reset() {
  std::lock_guard<std::mutex> guard(RegistryLock);
  for (ThreadSlot &slot : Slots) {
    slot.head.store(0, std::memory_order_relaxed);
  }
  SampleHead.store(0, std::memory_order_relaxed);
  FrameCount = 0;
  FrameOpenedAt = 0;
  TheSamplerStats.taken = 0;
  TheSamplerStats.missed = 0;
  TheSamplerStats.skipped = 0;
  TheSamplerStats.ticks = 0;
  TheSamplerStats.walks = 0;
  TheSamplerStats.barren = 0;
  SamplerRebase.fetch_add(1, std::memory_order_relaxed);
}

// --- public: reading ----------------------------------------------------------------------------

Window LastFrames(uint32_t count) {
  uint64_t oldest = 0;
  uint32_t have = 0;
  if (!HeldFrames(&oldest, &have)) {
    return Window();
  }
  const uint32_t want = count == 0 || count > have ? have : count;
  return WindowOverPositions(FrameCount - want, FrameCount - 1);
}

Window AroundFrame(uint64_t index, uint32_t pre, uint32_t post) {
  uint64_t oldest = 0;
  uint32_t have = 0;
  if (!HeldFrames(&oldest, &have)) {
    return Window();
  }
  // Find the ring position holding that frame index. A linear scan over at most FrameCapacity
  // entries, which is read-side and bounded; the indices are monotonic, so a binary search would
  // work too and is not worth the off-by-one risk for a query a human types.
  for (uint64_t position = oldest; position < FrameCount; ++position) {
    if (FrameRing[position % FrameCapacity].index != index) {
      continue;
    }
    const uint64_t first = position > pre ? position - pre : 0;
    return WindowOverPositions(first, position + post);
  }
  return Window();
}

Window CaptureWindow(uint32_t index) {
  Window window;
  if (index >= CaptureRing.size() || !CaptureRing[index].filled) {
    return window;
  }
  const Capture &capture = CaptureRing[index];
  window.t_begin = capture.t_begin;
  window.t_end = capture.t_end;
  window.frames = static_cast<uint32_t>(capture.frames.size());
  if (!capture.frames.empty()) {
    window.first_index = capture.frames.front().index;
    window.last_index = capture.frames.back().index;
  }
  window.capture = static_cast<int32_t>(index);
  window.valid = window.t_end > window.t_begin;
  return window;
}

std::vector<FrameSummary> FramesIn(const Window &window) {
  std::vector<FrameSummary> out;
  if (!window.valid) {
    return out;
  }
  if (window.capture >= 0) {
    const Capture &capture = CaptureRing[window.capture];
    out.reserve(capture.frames.size());
    for (const FrameRecord &record : capture.frames) {
      out.push_back(Summarize(record));
    }
    return out;
  }
  uint64_t oldest = 0;
  uint32_t have = 0;
  if (!HeldFrames(&oldest, &have)) {
    return out;
  }
  for (uint64_t position = oldest; position < FrameCount; ++position) {
    const FrameRecord &record = FrameRing[position % FrameCapacity];
    if (record.index >= window.first_index && record.index <= window.last_index) {
      out.push_back(Summarize(record));
    }
  }
  return out;
}

std::vector<FrameSummary> Frames(uint32_t count) {
  std::vector<FrameSummary> out;
  if (FrameRing == nullptr || FrameCount == 0) {
    return out;
  }
  const uint32_t have =
      static_cast<uint32_t>(FrameCount < FrameCapacity ? FrameCount : FrameCapacity);
  const uint32_t want = count == 0 || count > have ? have : count;
  out.reserve(want);
  for (uint32_t i = 0; i < want; ++i) {
    const uint64_t n = FrameCount - want + i;
    out.push_back(Summarize(FrameRing[n % FrameCapacity]));
  }
  return out;
}

std::vector<FrameSummary> WorstFrames(uint32_t count) {
  std::vector<FrameSummary> all = Frames(0);
  std::sort(all.begin(), all.end(),
            [](const FrameSummary &a, const FrameSummary &b) { return a.ms > b.ms; });
  if (count != 0 && all.size() > count) {
    all.resize(count);
  }
  return all;
}

std::vector<ZoneRow> Zones(const Window &window) {
  std::vector<ZoneRow> out;
  if (!window.valid) {
    return out;
  }

  struct Key {
    uint16_t site;
    uint32_t thread;
    bool operator==(const Key &o) const { return site == o.site && thread == o.thread; }
  };
  struct KeyHash {
    size_t operator()(const Key &k) const {
      return (static_cast<size_t>(k.thread) << 16) ^ k.site;
    }
  };
  std::unordered_map<Key, ZoneRow, KeyHash> rows;

  for (const Slice &slice : SliceEvents(window)) {
    // Events are properly nested within a thread, so a stack over start-ordered records gives
    // each one its direct children exactly - and it does so without relying on `depth`, which a
    // category turned off mid-window would make discontinuous.
    std::vector<std::pair<uint64_t, uint64_t *>> stack; // end tick, child accumulator
    std::vector<uint64_t> child(slice.events.size(), 0);
    for (size_t i = 0; i < slice.events.size(); ++i) {
      const Event &event = slice.events[i];
      if (event.kind != kScope) {
        continue;
      }
      while (!stack.empty() && stack.back().first <= event.t_begin) {
        stack.pop_back();
      }
      if (!stack.empty()) {
        *stack.back().second += event.dur;
      }
      stack.push_back({event.t_begin + event.dur, &child[i]});

      ZoneRow &row = rows[{event.site, slice.thread}];
      row.name = SiteName(event.site);
      row.thread = slice.thread;
      ++row.calls;
      const double ms = static_cast<double>(event.dur) * TicksToMs;
      row.incl_ms += ms;
      if (ms > row.max_ms) {
        row.max_ms = ms;
      }
    }
    for (size_t i = 0; i < slice.events.size(); ++i) {
      if (slice.events[i].kind != kScope) {
        continue;
      }
      ZoneRow &row = rows[{slice.events[i].site, slice.thread}];
      // Clamped as well as sorted for it: a window that begins mid-zone can hold a child whose
      // parent was trimmed off the front, and reporting 0 there beats reporting 2^64 ticks.
      const uint64_t dur = slice.events[i].dur;
      row.self_ms += static_cast<double>(dur > child[i] ? dur - child[i] : 0) * TicksToMs;
    }
  }

  out.reserve(rows.size());
  for (auto &entry : rows) {
    out.push_back(entry.second);
  }
  std::sort(out.begin(), out.end(),
            [](const ZoneRow &a, const ZoneRow &b) { return a.self_ms > b.self_ms; });
  return out;
}

std::vector<SampleRow> Samples(const Window &window) {
  std::vector<SampleRow> out;
  if (!window.valid) {
    return out;
  }
  // A capture holds its own copy; the live ring is walked by time. Both feed the same grouping
  // below, which is why this is a pointer pair rather than two loops.
  const Sample *live = nullptr;
  const std::vector<Sample> *captured = nullptr;
  uint32_t head = 0;
  uint32_t available = 0;
  if (window.capture >= 0) {
    captured = &CaptureRing[window.capture].samples;
    available = static_cast<uint32_t>(captured->size());
  } else {
    if (SampleRing == nullptr) {
      return out;
    }
    live = SampleRing;
    head = SampleHead.load(std::memory_order_acquire);
    available = head < SampleCapacity ? head : SampleCapacity;
  }

  struct Key {
    uint32_t address;
    uint16_t site;
    uint16_t thread;
    bool operator==(const Key &o) const {
      return address == o.address && site == o.site && thread == o.thread;
    }
  };
  struct KeyHash {
    size_t operator()(const Key &k) const {
      return k.address * 2654435761u ^ (static_cast<size_t>(k.site) << 3) ^ k.thread;
    }
  };
  std::unordered_map<Key, uint32_t, KeyHash> counts;
  for (uint32_t n = 0; n < available; ++n) {
    const Sample sample = captured != nullptr
                              ? (*captured)[n]
                              : live[(head - available + n) & (SampleCapacity - 1)];
    if (sample.t < window.t_begin || sample.t >= window.t_end) {
      continue;
    }
    ++counts[{sample.address, sample.site, sample.thread}];
  }

  out.reserve(counts.size());
  for (const auto &entry : counts) {
    SampleRow row;
    row.address = entry.first.address;
    row.samples = entry.second;
    row.zone = SiteName(entry.first.site);
    row.thread = entry.first.thread;
    out.push_back(row);
  }
  std::sort(out.begin(), out.end(),
            [](const SampleRow &a, const SampleRow &b) { return a.samples > b.samples; });
  return out;
}

std::vector<StackRow> Stacks(const Window &window, uint32_t limit) {
  std::vector<StackRow> out;
  if (!window.valid || StackDepth == 0) {
    return out;
  }
  const Sample *live = nullptr;
  const std::vector<Sample> *captured = nullptr;
  const uint32_t *stack_source = nullptr;
  uint32_t head = 0;
  uint32_t available = 0;
  if (window.capture >= 0) {
    const Capture &capture = CaptureRing[window.capture];
    captured = &capture.samples;
    stack_source = capture.stacks.empty() ? nullptr : capture.stacks.data();
    available = static_cast<uint32_t>(captured->size());
  } else {
    if (SampleRing == nullptr || StackArena == nullptr) {
      return out;
    }
    live = SampleRing;
    stack_source = StackArena;
    head = SampleHead.load(std::memory_order_acquire);
    available = head < SampleCapacity ? head : SampleCapacity;
  }
  if (stack_source == nullptr) {
    return out;
  }

  // Keyed on the frames themselves. A string key rather than a vector one because the map wants
  // a hash and eight bytes of hex per frame is cheaper to write than a good vector hash is to
  // get right; this is read-time code over a few thousand samples.
  std::unordered_map<std::string, StackRow> rows;
  std::string key;
  for (uint32_t n = 0; n < available; ++n) {
    // A capture's stacks are laid out in its own sample order, so the index is n; the live
    // arena is indexed by ring position, which is what the sampler wrote against.
    const uint32_t index =
        captured != nullptr ? n : ((head - available + n) & (SampleCapacity - 1));
    const Sample sample = captured != nullptr ? (*captured)[n] : live[index];
    if (sample.t < window.t_begin || sample.t >= window.t_end || sample.frames == 0) {
      continue;
    }
    const uint32_t *slots = stack_source + static_cast<size_t>(index) * StackDepth;
    const uint32_t depth = sample.frames < StackDepth ? sample.frames : StackDepth;
    key.clear();
    char buffer[12];
    for (uint32_t f = 0; f < depth; ++f) {
      std::snprintf(buffer, sizeof(buffer), "%08x", slots[f]);
      key += buffer;
    }
    // Same stack under a different zone, or on a different thread, is a different row.
    std::snprintf(buffer, sizeof(buffer), "|%04x|%02x", sample.site, sample.thread);
    key += buffer;
    StackRow &row = rows[key];
    if (row.samples == 0) {
      row.frames.assign(slots, slots + depth);
      row.zone = SiteName(sample.site);
      row.thread = sample.thread;
    }
    ++row.samples;
  }

  out.reserve(rows.size());
  for (auto &entry : rows) {
    out.push_back(std::move(entry.second));
  }
  std::sort(out.begin(), out.end(),
            [](const StackRow &a, const StackRow &b) { return a.samples > b.samples; });
  if (limit != 0 && out.size() > limit) {
    out.resize(limit);
  }
  return out;
}

std::vector<ThreadInfo> Threads() {
  std::vector<ThreadInfo> out;
  for (uint32_t i = 0; i < kMaxThreads; ++i) {
    ThreadSlot &slot = Slots[i];
    const uint32_t id = slot.thread_id.load(std::memory_order_acquire);
    if (id == 0) {
      continue;
    }
    ThreadInfo info;
    info.slot = i;
    info.id = id;
    info.name = slot.name;
    const uint32_t head = slot.head.load(std::memory_order_acquire);
    info.events = head < EventCapacity ? head : EventCapacity;
    info.lost = head > EventCapacity ? head - EventCapacity : 0;
    out.push_back(info);
  }
  return out;
}

std::vector<SiteInfo> SiteList() {
  std::vector<SiteInfo> out;
  const uint32_t count = SiteCount.load(std::memory_order_acquire);
  out.reserve(count);
  for (uint32_t i = 1; i < count; ++i) {
    out.push_back({Sites[i].name, Sites[i].cat});
  }
  return out;
}

const SamplerStats &Sampler() { return TheSamplerStats; }

double OverheadMs() {
  const std::vector<FrameSummary> last = Frames(1);
  if (last.empty()) {
    return 0.0;
  }
  // Priced rather than measured: the zone cost is calibrated at Arm, and this multiplies it by
  // what the last frame actually recorded. Timing the timer would be circular.
  return static_cast<double>(last.front().events * ZoneCostTicks + FrameMarkTicks) * TicksToMs;
}

std::string Describe(uint32_t address) {
  const ModuleRange *range = ModuleFor(address);
  if (range == nullptr) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%08x", address);
    return buffer;
  }
  const uint32_t rva = address - static_cast<uint32_t>(range->base);
  // Once per module, the first time a name is wanted for it. A miss is remembered, so a machine
  // with no maps pays one failed fopen per module for the whole session.
  if (SymbolAutoloadTried.find(range->name) == SymbolAutoloadTried.end()) {
    SymbolAutoloadTried[range->name] = true;
    const std::string dir = SymbolDirectory();
    if (!dir.empty()) {
      LoadSymbols(range->name, dir + range->name + ".sym");
    }
  }
  auto table = SymbolTables.find(range->name);
  if (table != SymbolTables.end()) {
    const std::vector<SymbolEntry> &entries = table->second.entries;
    auto it = std::upper_bound(entries.begin(), entries.end(), rva,
                               [](uint32_t v, const SymbolEntry &e) { return v < e.rva; });
    if (it != entries.begin()) {
      --it;
      if (it->size == 0 || rva < it->rva + it->size) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "+0x%x", rva - it->rva);
        return it->name + (rva == it->rva ? "" : buffer);
      }
    }
  }
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%s+0x%08x", range->name.c_str(), rva);
  return buffer;
}

std::string SymbolDirectory() {
  // From the host executable rather than from vfs::GameDir(), to keep this file free of every
  // dependency but Core.h - the profiler is one of the few src/ files that touches no game
  // memory and nothing here is worth giving that up for.
  char path[MAX_PATH] = {};
  if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0) {
    return std::string();
  }
  char *leaf = std::strrchr(path, '\\');
  if (leaf != nullptr) {
    *leaf = '\0';
  }
  return std::string(path) + "\\gkplus\\symbols\\";
}

SymbolLoad LoadSymbols(const std::string &module, const std::string &path) {
  SymbolLoad result;
  FILE *file = nullptr;
  if (fopen_s(&file, path.c_str(), "rb") != 0 || file == nullptr) {
    result.note = "cannot open " + path;
    return result;
  }
  SymbolTable table;
  uint64_t declared_size = 0;
  // Generous because a Ghidra database carries MSVC-decorated CRT template names: the longest
  // in gl.exe's is over 200 characters of angle brackets and commas. A line longer than this
  // splits across two fgets and the tail fails the scan, which drops one symbol rather than
  // producing a wrong one.
  char line[1024];
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    if (line[0] == '#') {
      unsigned long long value = 0;
      if (sscanf_s(line, "# file_size %llu", &value) == 1) {
        declared_size = value;
      }
      continue;
    }
    if (line[0] == '\n' || line[0] == '\r') {
      continue;
    }
    unsigned rva = 0;
    unsigned size = 0;
    // `%[^\r\n]` and not `%s`: the name is the rest of the line. It never contains whitespace in
    // practice (Ghidra substitutes `_` inside decorated names), but taking the remainder is what
    // the format promises and it costs nothing.
    char name[768] = {};
    if (sscanf_s(line, "%x %x %767[^\r\n]", &rva, &size, name,
                 static_cast<unsigned>(sizeof(name))) != 3) {
      continue;
    }
    table.entries.push_back({rva, size, name});
  }
  std::fclose(file);
  if (table.entries.empty()) {
    result.note = "no entries parsed from " + path;
    return result;
  }
  std::sort(table.entries.begin(), table.entries.end(),
            [](const SymbolEntry &a, const SymbolEntry &b) { return a.rva < b.rva; });

  // The map is against a *specific* build of the binary. A different one shifts every RVA, so
  // the names would be confidently wrong rather than absent - which is worse than hex.
  const uint64_t actual = ModuleFileSize(module);
  if (declared_size != 0 && actual != 0 && declared_size != actual) {
    result.stale = true;
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer),
                  "map was built against a %llu-byte %s; the loaded one is %llu bytes - "
                  "names may be wrong, regenerate with utils/symdump",
                  static_cast<unsigned long long>(declared_size), module.c_str(),
                  static_cast<unsigned long long>(actual));
    result.note = buffer;
  }
  result.entries = static_cast<uint32_t>(table.entries.size());
  result.ok = true;
  SymbolTables[module] = std::move(table);
  return result;
}

// --- public: trace export -------------------------------------------------------------------

namespace {

void AppendEscaped(std::string *out, const char *text) {
  for (const char *p = text; *p != '\0'; ++p) {
    if (*p == '"' || *p == '\\') {
      out->push_back('\\');
    }
    out->push_back(*p);
  }
}

const char *CatName(Cat cat) {
  switch (cat) {
  case Cat::Frame:
    return "frame";
  case Cat::Render:
    return "render";
  case Cat::Upload:
    return "upload";
  case Cat::Script:
    return "script";
  case Cat::Io:
    return "io";
  case Cat::Game:
    return "game";
  case Cat::Draw:
    return "draw";
  default:
    return "other";
  }
}

} // namespace

bool WriteTrace(const std::string &path, const Window &window, std::string *error) {
  if (!window.valid) {
    if (error != nullptr) {
      *error = "no frames in that window";
    }
    return false;
  }
  const uint64_t from = window.t_begin;
  FILE *file = nullptr;
  if (fopen_s(&file, path.c_str(), "wb") != 0 || file == nullptr) {
    if (error != nullptr) {
      *error = "cannot write " + path;
    }
    return false;
  }

  const double to_us = TicksToMs * 1000.0;
  std::fputs("{\"displayTimeUnit\":\"ms\",\"traceEvents\":[\n", file);
  bool first = true;
  auto separator = [&]() {
    if (!first) {
      std::fputs(",\n", file);
    }
    first = false;
  };

  for (const ThreadInfo &thread : Threads()) {
    separator();
    std::fprintf(file,
                 "{\"ph\":\"M\",\"name\":\"thread_name\",\"pid\":1,\"tid\":%u,"
                 "\"args\":{\"name\":\"%s\"}}",
                 thread.slot, thread.name);
  }

  for (const Slice &slice : SliceEvents(window)) {
    for (const Event &event : slice.events) {
      std::string name;
      AppendEscaped(&name, SiteName(event.site));
      const Cat cat = event.site < SiteCount.load(std::memory_order_acquire)
                          ? Sites[event.site].cat
                          : Cat::None;
      separator();
      if (event.kind == kScope) {
        std::fprintf(file,
                     "{\"ph\":\"X\",\"name\":\"%s\",\"cat\":\"%s\",\"pid\":1,\"tid\":%u,"
                     "\"ts\":%.3f,\"dur\":%.3f}",
                     name.c_str(), CatName(cat), slice.thread,
                     static_cast<double>(event.t_begin - from) * to_us,
                     static_cast<double>(event.dur) * to_us);
      } else if (event.kind == kInstant) {
        std::fprintf(file,
                     "{\"ph\":\"i\",\"name\":\"%s\",\"cat\":\"%s\",\"pid\":1,\"tid\":%u,"
                     "\"ts\":%.3f,\"s\":\"t\"}",
                     name.c_str(), CatName(cat), slice.thread,
                     static_cast<double>(event.t_begin - from) * to_us);
      } else {
        std::fprintf(file,
                     "{\"ph\":\"C\",\"name\":\"%s\",\"pid\":1,\"tid\":%u,\"ts\":%.3f,"
                     "\"args\":{\"value\":%u}}",
                     name.c_str(), slice.thread,
                     static_cast<double>(event.t_begin - from) * to_us, event.dur);
      }
    }
  }

  // Frames go on their own track, so a flame chart reads against the boundary rather than
  // against wall time - and a throttled frame is labelled as one in the chart itself.
  for (const FrameSummary &frame : FramesIn(window)) {
    separator();
    std::fprintf(file,
                 "{\"ph\":\"X\",\"name\":\"frame %llu%s\",\"cat\":\"frame\",\"pid\":1,"
                 "\"tid\":100,\"ts\":%.3f,\"dur\":%.3f}",
                 static_cast<unsigned long long>(frame.index),
                 frame.throttled ? " (throttled)" : "",
                 static_cast<double>(frame.t_begin - from) * to_us, frame.ms * 1000.0);
  }

  {
    const Sample *live = nullptr;
    const std::vector<Sample> *captured = nullptr;
    const uint32_t *stack_source = nullptr;
    uint32_t head = 0;
    uint32_t available = 0;
    if (window.capture >= 0) {
      const Capture &capture = CaptureRing[window.capture];
      captured = &capture.samples;
      stack_source = capture.stacks.empty() ? nullptr : capture.stacks.data();
      available = static_cast<uint32_t>(captured->size());
    } else if (SampleRing != nullptr) {
      live = SampleRing;
      stack_source = StackArena;
      head = SampleHead.load(std::memory_order_acquire);
      available = head < SampleCapacity ? head : SampleCapacity;
    }
    for (uint32_t n = 0; n < available; ++n) {
      const uint32_t index =
          captured != nullptr ? n : ((head - available + n) & (SampleCapacity - 1));
      const Sample sample = captured != nullptr ? (*captured)[n] : live[index];
      if (sample.t < window.t_begin || sample.t >= window.t_end) {
        continue;
      }
      std::string name;
      AppendEscaped(&name, Describe(sample.address).c_str());
      // The callers go in `args` as a semicolon-joined chain rather than through the trace
      // format's stackFrames table: this reads fine in Perfetto's detail pane and is one line
      // instead of an id-interning pass over the whole ring.
      std::string stack;
      if (stack_source != nullptr && StackDepth != 0 && sample.frames > 1) {
        const uint32_t *slots = stack_source + static_cast<size_t>(index) * StackDepth;
        const uint32_t depth = sample.frames < StackDepth ? sample.frames : StackDepth;
        for (uint32_t f = 1; f < depth; ++f) {
          if (f > 1) {
            stack += ';';
          }
          AppendEscaped(&stack, Describe(slots[f]).c_str());
        }
      }
      separator();
      std::fprintf(file,
                   "{\"ph\":\"i\",\"name\":\"%s\",\"cat\":\"sample\",\"pid\":1,\"tid\":%u,"
                   "\"ts\":%.3f,\"s\":\"t\",\"args\":{\"callers\":\"%s\"}}",
                   name.c_str(), sample.thread,
                   static_cast<double>(sample.t - from) * to_us, stack.c_str());
    }
  }

  std::fputs("\n]}\n", file);
  std::fclose(file);
  return true;
}

} // namespace gk::prof
