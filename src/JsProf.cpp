#include "Profiler.h"

#include "JsBindings.h"

#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

// The `prof` namespace (src/Profiler.h). Everything here is a read of a snapshot taken on this
// thread, so a script may ask for any of it from draw_gui or the REPL - both of which run at
// Present, which means the frame `prof.frame` reports is the one that has just ended.
//
// Every millisecond this prints is accompanied by `throttled`. That is deliberate and it is the
// one thing not to remove: a frame time taken under FIFO measures the monitor, not the game
// (vulkan_renderer_notes.md §4.79), and three sections of renderer work were lost to reading one
// without checking.

namespace gk::js {
namespace {

// Sites for `prof.mark`/`prof.count`/`prof.scope`. The profiler stores the name pointer rather
// than copying it, so the key has to outlive the process - an unordered_map's keys do, since its
// nodes never move.
std::unordered_map<std::string, uint16_t> ScriptSites;

uint16_t ScriptSite(const char *name, prof::Cat cat) {
  auto it = ScriptSites.find(name);
  if (it != ScriptSites.end()) {
    return it->second;
  }
  it = ScriptSites.emplace(name, uint16_t(0)).first;
  it->second = prof::RegisterSite(it->first.c_str(), cat);
  return it->second;
}

const char *CategoryName(prof::Cat cat) {
  switch (cat) {
  case prof::Cat::Frame:
    return "frame";
  case prof::Cat::Render:
    return "render";
  case prof::Cat::Upload:
    return "upload";
  case prof::Cat::Script:
    return "script";
  case prof::Cat::Io:
    return "io";
  case prof::Cat::Game:
    return "game";
  case prof::Cat::Draw:
    return "draw";
  default:
    return "other";
  }
}

// JsBindings' GetFloatProp reads a float; a trigger threshold is compared against a double
// frame time, and rounding a millisecond figure to 24 bits of mantissa before comparing it is
// the kind of thing that makes a threshold behave differently from what was typed.
bool GetDoubleProp(JSContext *ctx, JSValueConst obj, const char *name, double *out) {
  JSValue value = JS_GetPropertyStr(ctx, obj, name);
  if (JS_IsUndefined(value) || JS_IsNull(value) || JS_IsException(value)) {
    JS_FreeValue(ctx, value);
    return false;
  }
  double parsed = 0.0;
  const int ok = JS_ToFloat64(ctx, &parsed, value);
  JS_FreeValue(ctx, value);
  if (ok < 0) {
    return false;
  }
  *out = parsed;
  return true;
}

// Sets the property or unwinds the whole object - a half-built row would be worse than none.
bool Put(JSContext *ctx, JSValue obj, const char *key, JSValue value) {
  if (JS_IsException(value)) {
    return false;
  }
  return JS_SetPropertyStr(ctx, obj, key, value) >= 0;
}

JSValue NewFrameObject(JSContext *ctx, const prof::FrameSummary &frame) {
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  if (!Put(ctx, obj, "index", JS_NewInt64(ctx, static_cast<int64_t>(frame.index))) ||
      !Put(ctx, obj, "ms", JS_NewFloat64(ctx, frame.ms)) ||
      !Put(ctx, obj, "throttled", JS_NewBool(ctx, frame.throttled)) ||
      !Put(ctx, obj, "present", JS_NewString(ctx, frame.present)) ||
      !Put(ctx, obj, "events", JS_NewInt64(ctx, frame.events)) ||
      !Put(ctx, obj, "samples", JS_NewInt64(ctx, frame.samples))) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
  return obj;
}

JSValue NewFrameArray(JSContext *ctx, const std::vector<prof::FrameSummary> &frames) {
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) {
    return array;
  }
  for (size_t i = 0; i < frames.size(); ++i) {
    JSValue entry = NewFrameObject(ctx, frames[i]);
    if (JS_IsException(entry) ||
        JS_SetPropertyUint32(ctx, array, static_cast<uint32_t>(i), entry) < 0) {
      JS_FreeValue(ctx, array);
      return JS_EXCEPTION;
    }
  }
  return array;
}

// How many frames a query covers. Absent means "the whole ring", which is what a REPL user
// almost always wants; a number narrows it, and 1 is "the frame that just ended".
uint32_t FrameArg(JSContext *ctx, int argc, JSValueConst *argv, uint32_t fallback) {
  if (argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0])) {
    return fallback;
  }
  int32_t value = 0;
  if (JS_ToInt32(ctx, &value, argv[0]) < 0 || value < 0) {
    return fallback;
  }
  return static_cast<uint32_t>(value);
}

// What a query covers. Three spellings, because three questions get asked:
//
//   prof.zones(120)                             the last 120 frames
//   prof.zones({around: 4211, pre: 60, post: 20})   a frame that has already scrolled past
//   prof.zones({capture: 0})                    a window the trigger saved from being overwritten
//
// The second is the one that did not exist before: a stutter could be *seen* in prof.worst() and
// not profiled, because "the last N frames" was the only window expressible.
bool ReadWindow(JSContext *ctx, int argc, JSValueConst *argv, uint32_t fallback,
                prof::Window *out) {
  if (argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0])) {
    *out = prof::LastFrames(fallback);
    return true;
  }
  if (!JS_IsObject(argv[0])) {
    *out = prof::LastFrames(FrameArg(ctx, argc, argv, fallback));
    return true;
  }

  // GetInt32Prop's bool is "no exception pending", NOT "the property was there" - it leaves
  // *out alone when the property is absent, which is what makes a pre-seeded default work.
  // Reading it as presence made every object window resolve to capture 0, including one that
  // said {around: ...}. Sentinels, not return values.
  int32_t capture = -1;
  if (!GetInt32Prop(ctx, argv[0], "capture", &capture)) {
    return false;
  }
  if (capture >= 0) {
    *out = prof::CaptureWindow(static_cast<uint32_t>(capture));
    return true;
  }
  int64_t around = -1;
  if (!GetInt64Prop(ctx, argv[0], "around", &around)) {
    return false;
  }
  if (around >= 0) {
    int32_t pre = 60;
    int32_t post = 0;
    if (!GetInt32Prop(ctx, argv[0], "pre", &pre) ||
        !GetInt32Prop(ctx, argv[0], "post", &post)) {
      return false;
    }
    *out = prof::AroundFrame(static_cast<uint64_t>(around),
                             static_cast<uint32_t>(pre < 0 ? 0 : pre),
                             static_cast<uint32_t>(post < 0 ? 0 : post));
    return true;
  }
  int32_t last = static_cast<int32_t>(fallback);
  if (!GetInt32Prop(ctx, argv[0], "last", &last)) {
    return false;
  }
  *out = prof::LastFrames(static_cast<uint32_t>(last < 0 ? 0 : last));
  return true;
}

// --- accessors ---------------------------------------------------------------------------

JSValue GetEnabled(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, prof::Armed());
}

JSValue SetEnabled(JSContext *ctx, JSValueConst, JSValueConst value) {
  if (JS_ToBool(ctx, value)) {
    prof::Config config = prof::CurrentConfig();
    if (!prof::Arm(config)) {
      return JS_ThrowInternalError(ctx, "profiler could not allocate its rings");
    }
  } else {
    prof::Disarm();
  }
  return JS_UNDEFINED;
}

JSValue GetMask(JSContext *ctx, JSValueConst) {
  return JS_NewInt64(ctx, prof::ActiveMask.load(std::memory_order_relaxed));
}

JSValue SetMask(JSContext *ctx, JSValueConst, JSValueConst value) {
  int64_t mask = 0;
  if (JS_ToInt64(ctx, &mask, value) < 0) {
    return JS_EXCEPTION;
  }
  prof::SetMask(static_cast<uint32_t>(mask));
  return JS_UNDEFINED;
}

JSValue GetCategories(JSContext *ctx, JSValueConst) {
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  const prof::Cat all[] = {prof::Cat::Frame, prof::Cat::Render, prof::Cat::Upload,
                           prof::Cat::Script, prof::Cat::Io,    prof::Cat::Game,
                           prof::Cat::Draw};
  for (prof::Cat cat : all) {
    if (!Put(ctx, obj, CategoryName(cat), JS_NewInt64(ctx, uint32_t(cat)))) {
      JS_FreeValue(ctx, obj);
      return JS_EXCEPTION;
    }
  }
  return obj;
}

// What the rings are currently sized at. `configure` takes exactly these keys and leaves an
// absent one alone, so this is what a UI needs to show a field before anyone edits it - without
// it the only honest thing to display is the documented default, which is wrong the moment
// GKPLUS_PROFILER_HZ or an earlier configure() call has been through.
JSValue GetConfig(JSContext *ctx, JSValueConst) {
  const prof::Config config = prof::CurrentConfig();
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  if (!Put(ctx, obj, "mask", JS_NewInt64(ctx, config.mask)) ||
      !Put(ctx, obj, "events_per_thread", JS_NewInt64(ctx, config.events_per_thread)) ||
      !Put(ctx, obj, "frames", JS_NewInt64(ctx, config.frames)) ||
      !Put(ctx, obj, "sampler", JS_NewBool(ctx, config.sampler)) ||
      !Put(ctx, obj, "sampler_hz", JS_NewInt64(ctx, config.sampler_hz)) ||
      !Put(ctx, obj, "samples", JS_NewInt64(ctx, config.samples)) ||
      !Put(ctx, obj, "stacks", JS_NewBool(ctx, config.stacks)) ||
      !Put(ctx, obj, "stack_depth", JS_NewInt64(ctx, config.stack_depth)) ||
      !Put(ctx, obj, "captures", JS_NewInt64(ctx, config.captures)) ||
      !Put(ctx, obj, "capture_events", JS_NewInt64(ctx, config.capture_events)) ||
      !Put(ctx, obj, "capture_samples", JS_NewInt64(ctx, config.capture_samples))) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
  return obj;
}

JSValue GetFrame(JSContext *ctx, JSValueConst) {
  const std::vector<prof::FrameSummary> frames = prof::Frames(1);
  if (frames.empty()) {
    return JS_NULL;
  }
  return NewFrameObject(ctx, frames.front());
}

JSValue GetOverhead(JSContext *ctx, JSValueConst) {
  return JS_NewFloat64(ctx, prof::OverheadMs());
}

JSValue GetThreads(JSContext *ctx, JSValueConst) {
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) {
    return array;
  }
  const std::vector<prof::ThreadInfo> threads = prof::Threads();
  for (size_t i = 0; i < threads.size(); ++i) {
    JSValue entry = JS_NewObject(ctx);
    if (JS_IsException(entry) ||
        !Put(ctx, entry, "slot", JS_NewInt64(ctx, threads[i].slot)) ||
        !Put(ctx, entry, "id", JS_NewInt64(ctx, threads[i].id)) ||
        !Put(ctx, entry, "name", JS_NewString(ctx, threads[i].name)) ||
        !Put(ctx, entry, "events", JS_NewInt64(ctx, threads[i].events)) ||
        !Put(ctx, entry, "lost", JS_NewInt64(ctx, threads[i].lost)) ||
        JS_SetPropertyUint32(ctx, array, static_cast<uint32_t>(i), entry) < 0) {
      JS_FreeValue(ctx, array);
      return JS_EXCEPTION;
    }
  }
  return array;
}

JSValue GetSampler(JSContext *ctx, JSValueConst) {
  const prof::SamplerStats &stats = prof::Sampler();
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  if (!Put(ctx, obj, "running", JS_NewBool(ctx, stats.running)) ||
      !Put(ctx, obj, "hz", JS_NewInt64(ctx, stats.hz)) ||
      !Put(ctx, obj, "taken", JS_NewInt64(ctx, static_cast<int64_t>(stats.taken))) ||
      // A suspend or a GetThreadContext the OS refused. A few are normal - a thread exiting
      // between the registry read and the suspend is one - but a rising count means the profile
      // is missing time rather than that there was none.
      !Put(ctx, obj, "missed", JS_NewInt64(ctx, static_cast<int64_t>(stats.missed))) ||
      !Put(ctx, obj, "skipped", JS_NewInt64(ctx, static_cast<int64_t>(stats.skipped))) ||
      !Put(ctx, obj, "ticks", JS_NewInt64(ctx, static_cast<int64_t>(stats.ticks))) ||
      !Put(ctx, obj, "walks", JS_NewInt64(ctx, static_cast<int64_t>(stats.walks))) ||
      // Walks that yielded no caller at all. High against gl.exe is FPO and expected; high
      // against our own code is a defect.
      !Put(ctx, obj, "barren", JS_NewInt64(ctx, static_cast<int64_t>(stats.barren))) ||
      !Put(ctx, obj, "drift_ms", JS_NewFloat64(ctx, stats.drift_ms)) ||
      // Read this, not `hz`: a flat profile only stands for time if the sampling was uniform.
      !Put(ctx, obj, "effective_hz", JS_NewFloat64(ctx, stats.effective_hz))) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
  return obj;
}

JSValue GetTrigger(JSContext *ctx, JSValueConst) {
  const prof::TriggerConfig &trigger = prof::Trigger();
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  if (!Put(ctx, obj, "enabled", JS_NewBool(ctx, trigger.enabled)) ||
      !Put(ctx, obj, "min_ms", JS_NewFloat64(ctx, trigger.min_ms)) ||
      !Put(ctx, obj, "multiple", JS_NewFloat64(ctx, trigger.multiple)) ||
      !Put(ctx, obj, "pre", JS_NewInt64(ctx, trigger.pre)) ||
      !Put(ctx, obj, "post", JS_NewInt64(ctx, trigger.post)) ||
      // The median it compares against, so a threshold can be set from what the game is
      // actually doing rather than guessed at.
      !Put(ctx, obj, "baseline_ms", JS_NewFloat64(ctx, prof::BaselineMs()))) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
  return obj;
}

JSValue SetTrigger(JSContext *ctx, JSValueConst, JSValueConst value) {
  prof::TriggerConfig trigger = prof::Trigger();
  // `prof.trigger = true` is the whole configuration most of the time.
  if (JS_IsBool(value) || JS_IsNumber(value)) {
    trigger.enabled = JS_ToBool(ctx, value) != 0;
    prof::SetTrigger(trigger);
    return JS_UNDEFINED;
  }
  if (!JS_IsObject(value)) {
    return JS_ThrowTypeError(ctx, "prof.trigger takes a boolean or an options object");
  }
  // Each seeded with what the trigger already has, because GetInt32Prop leaves its out-param
  // alone for an absent property - so an omitted field keeps its value rather than becoming
  // whatever the previous field happened to read.
  GetDoubleProp(ctx, value, "min_ms", &trigger.min_ms);
  GetDoubleProp(ctx, value, "multiple", &trigger.multiple);
  int32_t pre = static_cast<int32_t>(trigger.pre);
  int32_t post = static_cast<int32_t>(trigger.post);
  if (!GetInt32Prop(ctx, value, "pre", &pre) || !GetInt32Prop(ctx, value, "post", &post)) {
    return JS_EXCEPTION;
  }
  trigger.pre = static_cast<uint32_t>(pre < 0 ? 0 : pre);
  trigger.post = static_cast<uint32_t>(post < 0 ? 0 : post);
  JSValue enabled = JS_GetPropertyStr(ctx, value, "enabled");
  // An options object with no `enabled` means "turn it on with these settings" - nobody
  // configures a trigger they do not intend to arm.
  trigger.enabled =
      JS_IsUndefined(enabled) || JS_IsNull(enabled) ? true : JS_ToBool(ctx, enabled) != 0;
  JS_FreeValue(ctx, enabled);
  prof::SetTrigger(trigger);
  return JS_UNDEFINED;
}

JSValue GetCaptures(JSContext *ctx, JSValueConst) {
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) {
    return array;
  }
  const std::vector<prof::CaptureInfo> captures = prof::Captures();
  for (size_t i = 0; i < captures.size(); ++i) {
    JSValue entry = JS_NewObject(ctx);
    if (JS_IsException(entry) ||
        !Put(ctx, entry, "index", JS_NewInt64(ctx, captures[i].index)) ||
        !Put(ctx, entry, "frame_index",
            JS_NewInt64(ctx, static_cast<int64_t>(captures[i].frame_index))) ||
        !Put(ctx, entry, "ms", JS_NewFloat64(ctx, captures[i].frame_ms)) ||
        !Put(ctx, entry, "baseline_ms", JS_NewFloat64(ctx, captures[i].baseline_ms)) ||
        !Put(ctx, entry, "throttled", JS_NewBool(ctx, captures[i].throttled)) ||
        !Put(ctx, entry, "frames", JS_NewInt64(ctx, captures[i].frames)) ||
        !Put(ctx, entry, "events", JS_NewInt64(ctx, captures[i].events)) ||
        !Put(ctx, entry, "samples", JS_NewInt64(ctx, captures[i].samples)) ||
        !Put(ctx, entry, "truncated", JS_NewBool(ctx, captures[i].truncated)) ||
        JS_SetPropertyUint32(ctx, array, static_cast<uint32_t>(i), entry) < 0) {
      JS_FreeValue(ctx, array);
      return JS_EXCEPTION;
    }
  }
  return array;
}

JSValue ClearCaptures(JSContext *, JSValueConst, int, JSValueConst *) {
  prof::ClearCaptures();
  return JS_UNDEFINED;
}

JSValue GetSites(JSContext *ctx, JSValueConst) {
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) {
    return array;
  }
  const std::vector<prof::SiteInfo> sites = prof::SiteList();
  for (size_t i = 0; i < sites.size(); ++i) {
    JSValue entry = JS_NewObject(ctx);
    if (JS_IsException(entry) ||
        !Put(ctx, entry, "name", JS_NewString(ctx, sites[i].name)) ||
        !Put(ctx, entry, "category", JS_NewString(ctx, CategoryName(sites[i].cat))) ||
        JS_SetPropertyUint32(ctx, array, static_cast<uint32_t>(i), entry) < 0) {
      JS_FreeValue(ctx, array);
      return JS_EXCEPTION;
    }
  }
  return array;
}

// --- methods -----------------------------------------------------------------------------

JSValue Frames(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  return NewFrameArray(ctx, prof::Frames(FrameArg(ctx, argc, argv, 0)));
}

JSValue Worst(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  return NewFrameArray(ctx, prof::WorstFrames(FrameArg(ctx, argc, argv, 10)));
}

JSValue Zones(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  prof::Window window;
  if (!ReadWindow(ctx, argc, argv, 60, &window)) {
    return JS_EXCEPTION;
  }
  const std::vector<prof::ZoneRow> rows = prof::Zones(window);
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) {
    return array;
  }
  // The window's own frame count, not the one asked for: a query that ran off the end of the
  // ring would otherwise divide by frames that were never recorded.
  const double divisor = window.frames != 0 ? static_cast<double>(window.frames) : 1.0;
  for (size_t i = 0; i < rows.size(); ++i) {
    JSValue entry = JS_NewObject(ctx);
    if (JS_IsException(entry) ||
        !Put(ctx, entry, "name", JS_NewString(ctx, rows[i].name)) ||
        !Put(ctx, entry, "thread", JS_NewInt64(ctx, rows[i].thread)) ||
        !Put(ctx, entry, "calls", JS_NewInt64(ctx, rows[i].calls)) ||
        !Put(ctx, entry, "incl_ms", JS_NewFloat64(ctx, rows[i].incl_ms)) ||
        !Put(ctx, entry, "self_ms", JS_NewFloat64(ctx, rows[i].self_ms)) ||
        !Put(ctx, entry, "max_ms", JS_NewFloat64(ctx, rows[i].max_ms)) ||
        // Per frame, because that is the number that compares against a frame time. The window
        // total is right there beside it for anyone who wants the other question.
        !Put(ctx, entry, "self_ms_per_frame", JS_NewFloat64(ctx, rows[i].self_ms / divisor)) ||
        JS_SetPropertyUint32(ctx, array, static_cast<uint32_t>(i), entry) < 0) {
      JS_FreeValue(ctx, array);
      return JS_EXCEPTION;
    }
  }
  return array;
}

JSValue Samples(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  prof::Window window;
  if (!ReadWindow(ctx, argc, argv, 60, &window)) {
    return JS_EXCEPTION;
  }
  const std::vector<prof::SampleRow> rows = prof::Samples(window);
  uint64_t total = 0;
  for (const prof::SampleRow &row : rows) {
    total += row.samples;
  }
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) {
    return array;
  }
  for (size_t i = 0; i < rows.size(); ++i) {
    JSValue entry = JS_NewObject(ctx);
    const std::string where = prof::Describe(rows[i].address);
    if (JS_IsException(entry) ||
        !Put(ctx, entry, "name", JS_NewString(ctx, where.c_str())) ||
        !Put(ctx, entry, "address", JS_NewInt64(ctx, rows[i].address)) ||
        !Put(ctx, entry, "samples", JS_NewInt64(ctx, rows[i].samples)) ||
        !Put(ctx, entry, "zone", JS_NewString(ctx, rows[i].zone)) ||
        !Put(ctx, entry, "thread", JS_NewInt64(ctx, rows[i].thread)) ||
        !Put(ctx, entry, "pct",
            JS_NewFloat64(ctx, total != 0 ? 100.0 * rows[i].samples / static_cast<double>(total)
                                          : 0.0)) ||
        JS_SetPropertyUint32(ctx, array, static_cast<uint32_t>(i), entry) < 0) {
      JS_FreeValue(ctx, array);
      return JS_EXCEPTION;
    }
  }
  return array;
}

JSValue Stacks(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  prof::Window window;
  if (!ReadWindow(ctx, argc, argv, 60, &window)) {
    return JS_EXCEPTION;
  }
  const std::vector<prof::StackRow> rows =
      prof::Stacks(window, FrameArg(ctx, argc - 1, argv + 1, 40));
  uint64_t total = 0;
  for (const prof::StackRow &row : rows) {
    total += row.samples;
  }
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) {
    return array;
  }
  for (size_t i = 0; i < rows.size(); ++i) {
    JSValue entry = JS_NewObject(ctx);
    JSValue stack = JS_NewArray(ctx);
    bool failed = JS_IsException(entry) || JS_IsException(stack);
    for (size_t f = 0; !failed && f < rows[i].frames.size(); ++f) {
      const std::string where = prof::Describe(rows[i].frames[f]);
      failed = JS_SetPropertyUint32(ctx, stack, static_cast<uint32_t>(f),
                                    JS_NewString(ctx, where.c_str())) < 0;
    }
    if (failed || !Put(ctx, entry, "stack", stack) ||
        !Put(ctx, entry, "samples", JS_NewInt64(ctx, rows[i].samples)) ||
        !Put(ctx, entry, "zone", JS_NewString(ctx, rows[i].zone)) ||
        !Put(ctx, entry, "thread", JS_NewInt64(ctx, rows[i].thread)) ||
        !Put(ctx, entry, "pct",
            JS_NewFloat64(ctx, total != 0 ? 100.0 * rows[i].samples / static_cast<double>(total)
                                          : 0.0)) ||
        JS_SetPropertyUint32(ctx, array, static_cast<uint32_t>(i), entry) < 0) {
      JS_FreeValue(ctx, array);
      return JS_EXCEPTION;
    }
  }
  return array;
}

JSValue Mark(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "prof.mark(name)");
  }
  const char *name = JS_ToCString(ctx, argv[0]);
  if (name == nullptr) {
    return JS_EXCEPTION;
  }
  prof::Instant(ScriptSite(name, prof::Cat::Script));
  JS_FreeCString(ctx, name);
  return JS_UNDEFINED;
}

JSValue Count(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "prof.count(name, value)");
  }
  const char *name = JS_ToCString(ctx, argv[0]);
  if (name == nullptr) {
    return JS_EXCEPTION;
  }
  int32_t value = 0;
  if (JS_ToInt32(ctx, &value, argv[1]) < 0) {
    JS_FreeCString(ctx, name);
    return JS_EXCEPTION;
  }
  prof::Counter(ScriptSite(name, prof::Cat::Script), static_cast<uint32_t>(value));
  JS_FreeCString(ctx, name);
  return JS_UNDEFINED;
}

// Times a callback and returns whatever it returned. The zone closes on the way out whether the
// callback returned or threw, because the Zone is an ordinary scoped object and JS_Call reports
// a throw as a return value rather than unwinding.
JSValue Scope(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 2 || !JS_IsFunction(ctx, argv[1])) {
    return JS_ThrowTypeError(ctx, "prof.scope(name, fn)");
  }
  const char *name = JS_ToCString(ctx, argv[0]);
  if (name == nullptr) {
    return JS_EXCEPTION;
  }
  const uint16_t site = ScriptSite(name, prof::Cat::Script);
  JS_FreeCString(ctx, name);
  prof::Zone zone(site, prof::Cat::Script);
  return JS_Call(ctx, argv[1], JS_UNDEFINED, 0, nullptr);
}

JSValue Trace(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "prof.trace(path[, frames])");
  }
  const char *path = JS_ToCString(ctx, argv[0]);
  if (path == nullptr) {
    return JS_EXCEPTION;
  }
  const std::string target = path;
  JS_FreeCString(ctx, path);
  prof::Window window;
  if (!ReadWindow(ctx, argc - 1, argv + 1, 0, &window)) {
    return JS_EXCEPTION;
  }
  std::string error;
  if (!prof::WriteTrace(target, window, &error)) {
    return JS_ThrowInternalError(ctx, "%s", error.c_str());
  }
  return JS_NewString(ctx, target.c_str());
}

JSValue Symbols(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "prof.symbols(module, path)");
  }
  const char *module = JS_ToCString(ctx, argv[0]);
  const char *path = module != nullptr ? JS_ToCString(ctx, argv[1]) : nullptr;
  if (path == nullptr) {
    JS_FreeCString(ctx, module);
    return JS_EXCEPTION;
  }
  const prof::SymbolLoad loaded = prof::LoadSymbols(module, path);
  JS_FreeCString(ctx, module);
  JS_FreeCString(ctx, path);
  if (!loaded.ok) {
    return JS_ThrowInternalError(ctx, "%s", loaded.note.c_str());
  }
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  // A stale map still loads - it is better than hex - but `stale` has to be visible, because
  // every name it produces would otherwise be confidently wrong rather than absent.
  if (!Put(ctx, obj, "entries", JS_NewInt64(ctx, loaded.entries)) ||
      !Put(ctx, obj, "stale", JS_NewBool(ctx, loaded.stale)) ||
      !Put(ctx, obj, "note", JS_NewString(ctx, loaded.note.c_str()))) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
  return obj;
}

JSValue SymbolDir(JSContext *ctx, JSValueConst) {
  return JS_NewString(ctx, prof::SymbolDirectory().c_str());
}

JSValue Configure(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  prof::Config config = prof::CurrentConfig();
  if (argc >= 1 && JS_IsObject(argv[0])) {
    // Each seeded with the value already in force and read into its OWN variable. GetInt32Prop
    // leaves its out-param untouched for an absent property and returns true - so one shared
    // `value` would have made {frames: 100} silently set events_per_thread, stack_depth and
    // every other field to 100 as well.
    struct Field {
      const char *name;
      uint32_t *slot;
    };
    const Field fields[] = {
        {"mask", &config.mask},
        {"events_per_thread", &config.events_per_thread},
        {"frames", &config.frames},
        {"sampler_hz", &config.sampler_hz},
        {"samples", &config.samples},
        {"stack_depth", &config.stack_depth},
        {"captures", &config.captures},
        {"capture_events", &config.capture_events},
        {"capture_samples", &config.capture_samples},
    };
    for (const Field &field : fields) {
      int32_t value = static_cast<int32_t>(*field.slot);
      if (!GetInt32Prop(ctx, argv[0], field.name, &value)) {
        return JS_EXCEPTION;
      }
      *field.slot = static_cast<uint32_t>(value < 0 ? 0 : value);
    }
    JSValue sampler = JS_GetPropertyStr(ctx, argv[0], "sampler");
    if (!JS_IsUndefined(sampler) && !JS_IsNull(sampler)) {
      config.sampler = JS_ToBool(ctx, sampler) != 0;
    }
    JS_FreeValue(ctx, sampler);
    JSValue stacks = JS_GetPropertyStr(ctx, argv[0], "stacks");
    if (!JS_IsUndefined(stacks) && !JS_IsNull(stacks)) {
      config.stacks = JS_ToBool(ctx, stacks) != 0;
    }
    JS_FreeValue(ctx, stacks);
  }
  // Re-arming with a bigger ring grows it; the profiler never shrinks or frees one, because a
  // recorder on the other thread could be inside it. See Profiler.h.
  if (!prof::Arm(config)) {
    return JS_ThrowInternalError(ctx, "profiler could not allocate its rings");
  }
  return JS_UNDEFINED;
}

JSValue Reset(JSContext *, JSValueConst, int, JSValueConst *) {
  prof::Reset();
  return JS_UNDEFINED;
}

const JSCFunctionListEntry ProfProps[] = {
    JS_CGETSET_DEF("enabled", GetEnabled, SetEnabled),
    JS_CGETSET_DEF("mask", GetMask, SetMask),
    JS_CGETSET_DEF("categories", GetCategories, nullptr),
    JS_CGETSET_DEF("config", GetConfig, nullptr),
    JS_CGETSET_DEF("frame", GetFrame, nullptr),
    JS_CGETSET_DEF("overhead_ms", GetOverhead, nullptr),
    JS_CGETSET_DEF("threads", GetThreads, nullptr),
    JS_CGETSET_DEF("sampler", GetSampler, nullptr),
    JS_CGETSET_DEF("sites", GetSites, nullptr),
    JS_CGETSET_DEF("symbol_dir", SymbolDir, nullptr),
    JS_CGETSET_DEF("trigger", GetTrigger, SetTrigger),
    JS_CGETSET_DEF("captures", GetCaptures, nullptr),
    JS_CFUNC_DEF("frames", 1, Frames),
    JS_CFUNC_DEF("worst", 1, Worst),
    JS_CFUNC_DEF("zones", 1, Zones),
    JS_CFUNC_DEF("samples", 1, Samples),
    JS_CFUNC_DEF("stacks", 2, Stacks),
    JS_CFUNC_DEF("mark", 1, Mark),
    JS_CFUNC_DEF("count", 2, Count),
    JS_CFUNC_DEF("scope", 2, Scope),
    JS_CFUNC_DEF("trace", 2, Trace),
    JS_CFUNC_DEF("symbols", 2, Symbols),
    JS_CFUNC_DEF("configure", 1, Configure),
    JS_CFUNC_DEF("clear_captures", 0, ClearCaptures),
    JS_CFUNC_DEF("reset", 0, Reset),
};

} // namespace

JSValue NewProfNamespace(JSContext *ctx) {
  return NewNamespace(ctx, ProfProps, static_cast<int>(std::size(ProfProps)));
}

} // namespace gk::js
