// The CPU profiler (`prof`), as ImGui.
//
// Its own module for the same two reasons render-panel.mjs is: main.mjs is a
// teaching example and this would swamp it, and a panel you can paste into your
// own entry module beside your own code is more use than one you have to cut out
// of somebody else's. It draws into whatever window the caller has open - no
// Begin/End of its own - so `draw_prof_panel(ImGui)` composes with anything.
//
// ## Two rules, neither of them a style preference
//
// **Query on a cadence, and only what is on screen.** `draw_gui` runs every
// frame, and `prof.zones` / `prof.samples` / `prof.stacks` each walk a window of
// the event ring and build one JS object per row - thousands, for a sampled
// profile. Reading those sixty times a second would make the profiler the most
// expensive thing in the frame it is measuring, and `prof.overhead_ms` would not
// even show it, because the cost is on our side of the binding. So every query
// goes through `snapshot()` below, which refreshes on an interval (or a button),
// and a collapsed section queries nothing at all.
//
// The one exception is `prof.frame`, which is a single object for the frame that
// has just ended. That is cheap enough to read every frame, and reading it every
// frame is what builds the graph.
//
// **Write a setting only when the widget says it changed.** `prof.enabled`,
// `prof.configure` and `prof.trigger` all re-arm the profiler - `configure` can
// allocate a new set of rings - and `prof.reset` empties the history. None of
// them are things to do sixty times a second.
//
// ## What the panel puts in front of you on purpose
//
// `throttled` and `overhead_ms`, at the top, before anything else. A frame time
// taken while the presenter was waiting for a vertical blank measures the
// monitor rather than the game (vulkan_renderer_notes.md 4.79 struck out three
// sections of renderer work that were measured against exactly that), and a
// profile is only worth reading once you know what carrying it cost.

import { prof } from "gk";

/** How many frame times the graph keeps. The ring the profiler itself keeps is
 *  far longer - this is only what has been observed while the overlay was open,
 *  which is why a gap in the frame index reseeds it from `prof.frames`. */
const HISTORY = 240;

const RED = { x: 1.0, y: 0.4, z: 0.35, w: 1 };
const AMBER = { x: 1.0, y: 0.8, z: 0.3, w: 1 };
const GREEN = { x: 0.5, y: 0.9, z: 0.5, w: 1 };
const DIM = { x: 0.6, y: 0.6, z: 0.6, w: 1 };

/** @param {number} x */
const ms = (x) => (Number.isFinite(x) ? x.toFixed(2) : "-");
/** @param {number} x */
const pct = (x) => (Number.isFinite(x) ? x.toFixed(1) + "%" : "-");

// --- the frame-time graph ----------------------------------------------------

/** @type {number[]} */
let history = [];
/** The graph's top of scale in ms, or 0 to fit it to the tallest frame in view. */
let graphMax = 0;
/** Whether each frame in `history` was waiting for a blank. Kept beside it
 *  rather than derived, because "the graph is 100% vsync" is the single most
 *  useful thing this panel can tell you and it is invisible in the times.
 *  @type {boolean[]} */
let throttledHistory = [];
let lastFrameIndex = -1;

/** Reads the frame that has just ended and appends it. Returns it, or null when
 *  the profiler is not recording.
 *  @returns {import("gk").ProfFrame | null} */
function observeFrame() {
  const frame = prof.frame;
  if (!frame) {
    history = [];
    throttledHistory = [];
    lastFrameIndex = -1;
    return null;
  }
  if (frame.index === lastFrameIndex) {
    return frame; // drawn twice in one frame; nothing new to append
  }
  if (lastFrameIndex < 0 || frame.index !== lastFrameIndex + 1) {
    // First open, or the overlay was closed for a while - take the gap out of
    // the profiler's own ring rather than plotting a discontinuity as data.
    const seed = prof.frames(HISTORY);
    history = seed.map((f) => f.ms);
    throttledHistory = seed.map((f) => f.throttled);
  } else {
    history.push(frame.ms);
    throttledHistory.push(frame.throttled);
    if (history.length > HISTORY) {
      history.shift();
      throttledHistory.shift();
    }
  }
  lastFrameIndex = frame.index;
  return frame;
}

// --- the query window --------------------------------------------------------

/** @type {"last" | "around" | "capture"} */
let windowMode = "last";
let lastCount = 60;
let aroundFrame = 0;
let aroundPre = 60;
let aroundPost = 0;
let captureIndex = 0;

/** @returns {import("gk").ProfWindow} */
function queryWindow() {
  if (windowMode === "around") {
    return { around: aroundFrame, pre: aroundPre, post: aroundPost };
  }
  if (windowMode === "capture") {
    return { capture: captureIndex };
  }
  return { last: lastCount };
}

function windowLabel() {
  if (windowMode === "around") {
    return `frame ${aroundFrame} -${aroundPre}/+${aroundPost}`;
  }
  if (windowMode === "capture") {
    return `capture ${captureIndex}`;
  }
  return `last ${lastCount} frames`;
}

// --- the snapshot ------------------------------------------------------------
//
// One cache per query, filled the first time a section that needs it is drawn
// after an invalidation. A section that stays collapsed therefore costs nothing,
// which is what makes it safe to leave the panel open.

/** @type {{zones: import("gk").ProfZone[] | null,
 *          samples: import("gk").ProfSample[] | null,
 *          stacks: import("gk").ProfStack[] | null,
 *          worst: import("gk").ProfFrame[] | null,
 *          error: string | null}} */
const cache = { zones: null, samples: null, stacks: null, worst: null, error: null };

let autoRefresh = true;
let refreshEvery = 30;
let sinceRefresh = 0;
let stackLimit = 20;
let sampleLimit = 25;

function invalidate() {
  cache.zones = null;
  cache.samples = null;
  cache.stacks = null;
  cache.worst = null;
  cache.error = null;
  sinceRefresh = 0;
}

/** Runs one query into its cache slot. Every read goes through here, because a
 *  throw out of `draw_gui` disables the overlay for the rest of the session and
 *  a profiler panel is not worth that.
 *  @template {"zones" | "samples" | "stacks" | "worst"} K
 *  @param {K} key
 *  @param {() => any} read
 */
function query(key, read) {
  if (cache[key] === null) {
    try {
      cache[key] = read();
    } catch (error) {
      cache[key] = [];
      cache.error = String(error);
    }
  }
  return cache[key];
}

// --- zone sorting ------------------------------------------------------------

/** @type {Array<{label: string, key: keyof import("gk").ProfZone}>} */
const ZONE_SORTS = [
  { label: "self / frame", key: "self_ms_per_frame" },
  { label: "self total", key: "self_ms" },
  { label: "inclusive", key: "incl_ms" },
  { label: "worst call", key: "max_ms" },
  { label: "calls", key: "calls" },
];
let zoneSort = 0;

// --- pending configuration ---------------------------------------------------
//
// `prof.configure` re-arms and can allocate a new set of rings, so it is applied
// by a button rather than by a slider. These hold what has been typed but not
// applied yet.

/** @type {{[key: string]: number | boolean}} */
let pendingConfig = {};
/** @type {string | null} */
let configNote = null;

/** What a field shows: what has been typed, or what the profiler is actually
 *  running with. Never a documented default - `prof.config` exists so that a
 *  field cannot claim 1000 Hz while the sampler runs at whatever
 *  GKPLUS_PROFILER_HZ said.
 *  @param {keyof import("gk").ProfConfig} key */
function pending(key) {
  const current = prof.config[key];
  return pendingConfig[key] === undefined ? current : pendingConfig[key];
}

let tracePath = "gkplus-trace.json";
let symbolModule = "gl.exe";
let symbolPath = "";
/** @type {string | null} */
let note = null;

/** The trigger's current settings. The property is written with a partial
 *  object and read back as a whole one, which is more than a JSDoc property type
 *  can say - hence the cast.
 *  @returns {import("gk").ProfTrigger} */
function readTrigger() {
  return /** @type {import("gk").ProfTrigger} */ (prof.trigger);
}

/** Writes a patch WITHOUT arming the trigger as a side effect: an options object
 *  with no `enabled` means "turn it on with these settings", so dragging a
 *  slider on a disarmed trigger would arm it.
 *  @param {Partial<import("gk").ProfTrigger>} patch */
function setTrigger(patch) {
  prof.trigger = { enabled: readTrigger().enabled, ...patch };
}

/**
 * Draw the whole panel into the caller's window.
 *
 * @param {ImGui} ImGui
 */
export function draw_prof_panel(ImGui) {
  // Observed every frame, open or closed - one object, and it is what keeps the
  // graph continuous across a collapsed header.
  const frame = observeFrame();

  if (!ImGui.CollapsingHeader("Profiler")) {
    return;
  }

  if (!prof.enabled) {
    ImGui.TextWrapped(
      "Not recording. Arming allocates the event, frame and sample rings; they " +
        "are never freed afterwards, because a recorder on the other thread may " +
        "be inside one. GKPLUS_PROFILER=1 arms it at boot instead."
    );
    if (ImGui.Button("Arm the profiler")) {
      prof.enabled = true;
      invalidate();
    }
    return;
  }

  drawSummary(ImGui, frame);
  drawGraph(ImGui);
  drawWindowPicker(ImGui);

  if (cache.error) {
    ImGui.TextColored(RED, cache.error);
  }

  ImGui.PushItemWidth(150);
  drawZones(ImGui);
  drawSamples(ImGui);
  drawStacks(ImGui);
  drawWorst(ImGui);
  drawTrigger(ImGui);
  drawHealth(ImGui);
  drawRecording(ImGui);
  ImGui.PopItemWidth();

  if (note) {
    ImGui.SeparatorText("Last action");
    if (ImGui.SmallButton("Dismiss")) {
      note = null;
    } else {
      ImGui.TextWrapped(note);
    }
  }
}

// --- sections ----------------------------------------------------------------

/** @param {ImGui} ImGui @param {import("gk").ProfFrame | null} frame */
function drawSummary(ImGui, frame) {
  if (!frame) {
    ImGui.TextColored(DIM, "armed, but no frame has ended yet");
    return;
  }

  ImGui.Text(`frame ${frame.index}: ${ms(frame.ms)} ms`);
  ImGui.SameLine();
  ImGui.TextColored(DIM, `(${(1000 / Math.max(frame.ms, 0.001)).toFixed(0)} fps)`);
  ImGui.SameLine();
  if (frame.throttled) {
    ImGui.TextColored(AMBER, `[${frame.present}, waiting for vsync]`);
    ImGui.SetItemTooltip(
      "This frame time measures the MONITOR, not the game: the presenter blocked " +
        "for a vertical blank. Every millisecond below is still real, but the frame " +
        "total they are compared against is not, and an A/B across it says nothing. " +
        "GKPLUS_VK_PRESENT_MODE=immediate is the way out."
    );
  } else {
    ImGui.TextColored(GREEN, `[${frame.present}]`);
  }

  // Read this before believing anything else on the panel - it is what the
  // instrumentation cost the frame it is describing.
  const overhead = prof.overhead_ms;
  const share = frame.ms > 0 ? (100 * overhead) / frame.ms : 0;
  ImGui.TextColored(
    share > 5 ? RED : share > 2 ? AMBER : DIM,
    `overhead ${ms(overhead)} ms (${pct(share)} of the frame), ` +
      `${frame.events} events, ${frame.samples} samples`
  );
  ImGui.SetItemTooltip(
    "Priced from a zone cost calibrated when the profiler was armed, against what " +
      "this frame actually recorded. Past a few percent the profile is measuring " +
      "itself - drop a category out of the mask, `draw` first."
  );
}

/** @param {ImGui} ImGui */
function drawGraph(ImGui) {
  if (history.length === 0) {
    return;
  }
  let max = 0;
  let total = 0;
  let throttled = 0;
  for (let i = 0; i < history.length; i++) {
    if (history[i] > max) {
      max = history[i];
    }
    total += history[i];
    if (throttledHistory[i]) {
      throttled += 1;
    }
  }
  const mean = total / history.length;
  const overlay =
    `${history.length} frames  avg ${ms(mean)}  max ${ms(max)}` +
    (throttled > 0 ? `  vsync ${pct((100 * throttled) / history.length)}` : "");

  // The top of scale is the 95th percentile, NOT the tallest bar. Fitting to the
  // max is what a graph like this obviously ought to do and it is wrong here: one
  // level-load frame of 121 ms against a 30 ms baseline flattens every other bar
  // into a stripe, so the stutter you went looking for is the only thing you can
  // no longer see the shape of. The outlier still clips off the top, and `max` is
  // in the overlay text - which is the part worth reading numerically anyway.
  const ranked = history.slice().sort((a, b) => a - b);
  const p95 = ranked[Math.min(ranked.length - 1, Math.floor(ranked.length * 0.95))];
  // Zero at the bottom, always: an auto-fitted floor makes a steady 5 ms and a
  // steady 25 ms draw exactly the same picture.
  ImGui.PlotHistogram("##prof-frames", history, {
    scale_min: 0,
    scale_max: graphMax > 0 ? graphMax : Math.max(p95 * 1.25, 1),
    overlay,
    size: { x: 0, y: 70 },
  });
  const scale = ImGui.InputFloat("graph max (ms)", graphMax, { step: 1, format: "%.1f" });
  if (scale.changed) {
    graphMax = Math.max(0, scale.value);
  }
  ImGui.SetItemTooltip(
    "0 fits the graph to the 95th percentile rather than the tallest frame, so a " +
      "single load spike cannot flatten the trace. Bars above it clip; `max` in the " +
      "overlay is the real figure."
  );
  if (throttled === history.length) {
    ImGui.TextColored(
      AMBER,
      "Every frame here was waiting for a blank - this graph is the refresh rate."
    );
  }
}

/** @param {ImGui} ImGui */
function drawWindowPicker(ImGui) {
  ImGui.SeparatorText(`Window: ${windowLabel()}`);

  /** @type {Array<"last" | "around" | "capture">} */
  const modes = ["last", "around", "capture"];
  for (const mode of modes) {
    if (mode !== "last") {
      ImGui.SameLine();
    }
    if (ImGui.RadioButton(mode, windowMode === mode) && windowMode !== mode) {
      windowMode = mode;
      invalidate();
    }
  }
  ImGui.SetItemTooltip(
    "`around` is the one that matters for a stutter: the frame ring reaches far " +
      "further back than the event ring, so `worst` finds a slow frame that 'the " +
      "last N frames' cannot address. `capture` reads a window the trigger saved."
  );

  ImGui.PushItemWidth(120);
  if (windowMode === "last") {
    const n = ImGui.InputInt("frames", lastCount);
    if (n.changed) {
      lastCount = Math.max(1, n.value);
      invalidate();
    }
  } else if (windowMode === "around") {
    const at = ImGui.InputInt("frame index", aroundFrame);
    if (at.changed) {
      aroundFrame = Math.max(0, at.value);
      invalidate();
    }
    const pre = ImGui.InputInt("pre", aroundPre);
    ImGui.SameLine();
    const post = ImGui.InputInt("post", aroundPost);
    if (pre.changed || post.changed) {
      aroundPre = Math.max(0, pre.value);
      aroundPost = Math.max(0, post.value);
      invalidate();
    }
  } else {
    const at = ImGui.InputInt("capture index", captureIndex);
    if (at.changed) {
      captureIndex = Math.max(0, at.value);
      invalidate();
    }
    ImGui.SetItemTooltip(
      "The capture's own `index`, which is a handle - not its position in the " +
        "list, because the ring drops the oldest and the two diverge."
    );
  }
  ImGui.PopItemWidth();

  if (ImGui.Button("Refresh")) {
    invalidate();
  }
  ImGui.SameLine();
  const auto = ImGui.Checkbox("auto", autoRefresh);
  if (auto.changed) {
    autoRefresh = auto.value;
  }
  ImGui.SameLine();
  ImGui.PushItemWidth(100);
  const every = ImGui.InputInt("every (frames)", refreshEvery);
  if (every.changed) {
    refreshEvery = Math.max(1, every.value);
  }
  ImGui.PopItemWidth();

  // A capture and an `around` window are static - re-running the same query
  // against frames that cannot change would only cost time.
  sinceRefresh += 1;
  if (autoRefresh && windowMode === "last" && sinceRefresh >= refreshEvery) {
    invalidate();
  }
}

/** @param {ImGui} ImGui */
function drawZones(ImGui) {
  if (!ImGui.TreeNode("Zones")) {
    return;
  }
  ImGui.TextWrapped(
    "Instrumented call sites (GK_ZONE) over the window. `self` is inclusive minus " +
      "the direct children, exact rather than estimated, and the same site recorded " +
      "from both game threads is two rows - they are different work."
  );

  const sort = ImGui.Combo("sort by", zoneSort, ZONE_SORTS.map((s) => s.label));
  if (sort.changed) {
    zoneSort = sort.current_item;
  }

  const rows = /** @type {import("gk").ProfZone[]} */ (
    query("zones", () => prof.zones(queryWindow()))
  );
  const key = ZONE_SORTS[zoneSort].key;
  const sorted = rows.slice().sort((a, b) => Number(b[key]) - Number(a[key]));
  const names = threadNames();

  const flags =
    ImGui.TableFlags.RowBg |
    ImGui.TableFlags.Borders |
    ImGui.TableFlags.Resizable |
    ImGui.TableFlags.ScrollY;
  if (ImGui.BeginTable("prof-zones", 7, { flags, outerSize: { x: 0, y: 220 } })) {
    ImGui.TableSetupScrollFreeze(0, 1);
    ImGui.TableSetupColumn("zone");
    ImGui.TableSetupColumn("thread", { flags: ImGui.TableColumnFlags.WidthFixed, initWidthOrWeight: 64 });
    ImGui.TableSetupColumn("self/frame", { flags: ImGui.TableColumnFlags.WidthFixed, initWidthOrWeight: 74 });
    ImGui.TableSetupColumn("self", { flags: ImGui.TableColumnFlags.WidthFixed, initWidthOrWeight: 62 });
    ImGui.TableSetupColumn("incl", { flags: ImGui.TableColumnFlags.WidthFixed, initWidthOrWeight: 62 });
    ImGui.TableSetupColumn("worst", { flags: ImGui.TableColumnFlags.WidthFixed, initWidthOrWeight: 62 });
    ImGui.TableSetupColumn("calls", { flags: ImGui.TableColumnFlags.WidthFixed, initWidthOrWeight: 62 });
    ImGui.TableHeadersRow();
    for (const row of sorted) {
      ImGui.TableNextRow();
      ImGui.TableNextColumn();
      ImGui.Text(row.name);
      ImGui.TableNextColumn();
      ImGui.TextColored(DIM, names[row.thread] ?? String(row.thread));
      ImGui.TableNextColumn();
      ImGui.Text(ms(row.self_ms_per_frame));
      ImGui.TableNextColumn();
      ImGui.Text(ms(row.self_ms));
      ImGui.TableNextColumn();
      ImGui.Text(ms(row.incl_ms));
      ImGui.TableNextColumn();
      ImGui.Text(ms(row.max_ms));
      ImGui.TableNextColumn();
      ImGui.Text(String(row.calls));
    }
    ImGui.EndTable();
  }
  if (rows.length === 0) {
    ImGui.TextColored(DIM, "no zones in this window");
  }
  ImGui.TreePop();
}

/** @param {ImGui} ImGui */
function drawSamples(ImGui) {
  if (!ImGui.TreeNode("Sampled profile")) {
    return;
  }
  ImGui.TextWrapped(
    "Where the sampling thread found each game thread, joined to the instrumented " +
      "zones by the zone each sample landed inside. This is the half that sees code " +
      "nobody instrumented."
  );
  const limit = ImGui.InputInt("rows", sampleLimit);
  if (limit.changed) {
    sampleLimit = Math.max(1, limit.value);
  }

  const rows = /** @type {import("gk").ProfSample[]} */ (
    query("samples", () => prof.samples(queryWindow()))
  );
  const names = threadNames();
  const flags =
    ImGui.TableFlags.RowBg |
    ImGui.TableFlags.Borders |
    ImGui.TableFlags.Resizable |
    ImGui.TableFlags.ScrollY;
  if (ImGui.BeginTable("prof-samples", 5, { flags, outerSize: { x: 0, y: 200 } })) {
    ImGui.TableSetupScrollFreeze(0, 1);
    ImGui.TableSetupColumn("%", { flags: ImGui.TableColumnFlags.WidthFixed, initWidthOrWeight: 48 });
    ImGui.TableSetupColumn("samples", { flags: ImGui.TableColumnFlags.WidthFixed, initWidthOrWeight: 62 });
    ImGui.TableSetupColumn("where");
    ImGui.TableSetupColumn("zone", { flags: ImGui.TableColumnFlags.WidthFixed, initWidthOrWeight: 120 });
    ImGui.TableSetupColumn("thread", { flags: ImGui.TableColumnFlags.WidthFixed, initWidthOrWeight: 64 });
    ImGui.TableHeadersRow();
    for (const row of rows.slice(0, sampleLimit)) {
      ImGui.TableNextRow();
      ImGui.TableNextColumn();
      ImGui.Text(pct(row.pct));
      ImGui.TableNextColumn();
      ImGui.Text(String(row.samples));
      ImGui.TableNextColumn();
      ImGui.Text(row.name);
      ImGui.TableNextColumn();
      ImGui.TextColored(DIM, row.zone || "-");
      ImGui.TableNextColumn();
      ImGui.TextColored(DIM, names[row.thread] ?? String(row.thread));
    }
    ImGui.EndTable();
  }
  if (rows.length === 0) {
    ImGui.TextColored(DIM, "no samples - is the sampler running?");
  } else if (rows.length > sampleLimit) {
    ImGui.TextColored(DIM, `${rows.length - sampleLimit} more rows not shown`);
  }
  ImGui.TreePop();
}

/** @param {ImGui} ImGui */
function drawStacks(ImGui) {
  if (!ImGui.TreeNode("Call stacks")) {
    return;
  }
  ImGui.TextWrapped(
    "Distinct stacks, most frequent first, leaf first within each. Empty unless the " +
      "sampler was armed with stacks (GKPLUS_PROFILER=stacks, or Recording below). " +
      "Install a symbol map to read gl.exe's half as names rather than RVAs."
  );
  const limit = ImGui.InputInt("stacks", stackLimit);
  if (limit.changed) {
    stackLimit = Math.max(1, limit.value);
    cache.stacks = null;
  }

  const rows = /** @type {import("gk").ProfStack[]} */ (
    query("stacks", () => prof.stacks(queryWindow(), stackLimit))
  );
  if (rows.length === 0) {
    ImGui.TextColored(DIM, "no stacks recorded in this window");
    ImGui.TreePop();
    return;
  }
  if (ImGui.BeginChild("prof-stacks", { size: { x: 0, y: 240 } })) {
    for (let i = 0; i < rows.length; i++) {
      const row = rows[i];
      const head = row.stack.length > 0 ? row.stack[0] : "(empty)";
      // The index is in the label so two identical leaves stay two nodes: an
      // ImGui id is its label, and a collision merges them into one.
      if (ImGui.TreeNode(`${pct(row.pct)}  ${head}##stack${i}`)) {
        ImGui.TextColored(DIM, `${row.samples} samples, zone ${row.zone || "-"}`);
        for (const level of row.stack) {
          ImGui.Text(level);
        }
        ImGui.TreePop();
      }
    }
  }
  ImGui.EndChild();
  ImGui.TreePop();
}

/** @param {ImGui} ImGui */
function drawWorst(ImGui) {
  if (!ImGui.TreeNode("Worst frames")) {
    return;
  }
  ImGui.TextWrapped(
    "The slowest frames still in the frame ring, worst first. The frame ring is far " +
      "cheaper per entry than the event ring, so this reaches minutes back - much " +
      "further than anything that can still be profiled. Take the index to `around`."
  );
  const rows = /** @type {import("gk").ProfFrame[]} */ (query("worst", () => prof.worst(12)));
  const flags = ImGui.TableFlags.RowBg | ImGui.TableFlags.Borders;
  if (ImGui.BeginTable("prof-worst", 5, { flags })) {
    ImGui.TableSetupColumn("frame");
    ImGui.TableSetupColumn("ms");
    ImGui.TableSetupColumn("events");
    ImGui.TableSetupColumn("vsync");
    ImGui.TableSetupColumn("");
    ImGui.TableHeadersRow();
    for (const row of rows) {
      ImGui.TableNextRow();
      ImGui.TableNextColumn();
      ImGui.Text(String(row.index));
      ImGui.TableNextColumn();
      ImGui.Text(ms(row.ms));
      ImGui.TableNextColumn();
      ImGui.Text(`${row.events} / ${row.samples}`);
      ImGui.TableNextColumn();
      if (row.throttled) {
        ImGui.TextColored(AMBER, "yes");
      } else {
        ImGui.TextColored(DIM, "no");
      }
      ImGui.TableNextColumn();
      if (ImGui.SmallButton(`profile##worst${row.index}`)) {
        windowMode = "around";
        aroundFrame = row.index;
        invalidate();
      }
    }
    ImGui.EndTable();
  }
  ImGui.TreePop();
}

/** @param {ImGui} ImGui */
function drawTrigger(ImGui) {
  if (!ImGui.TreeNode("Trigger and captures")) {
    return;
  }
  const trigger = readTrigger();
  ImGui.TextWrapped(
    "The flight recorder. A frame past BOTH the floor and the multiple of the " +
      "running median copies its surrounding window out of the rings before they " +
      "can overwrite it - which is the only way to profile a stutter rarer than the " +
      "event ring's reach, because by the time you have seen one it is gone."
  );

  const on = ImGui.Checkbox("armed", trigger.enabled);
  if (on.changed) {
    prof.trigger = { enabled: on.value };
  }
  if (prof.frame?.throttled) {
    ImGui.TextColored(
      AMBER,
      "Unthrottle first: under FIFO both the threshold and the baseline are the monitor."
    );
  }

  const min = ImGui.InputFloat("min ms", trigger.min_ms, { step: 1, format: "%.1f" });
  if (min.changed) {
    setTrigger({ min_ms: Math.max(0, min.value) });
  }
  ImGui.SetItemTooltip("An absolute floor, so a fast steady game does not trip on jitter.");
  const mult = ImGui.InputFloat("x median", trigger.multiple, { step: 0.5, format: "%.2f" });
  if (mult.changed) {
    setTrigger({ multiple: Math.max(0, mult.value) });
  }
  ImGui.SetItemTooltip(
    `A multiple of the running median, so one number is not wrong for every scene. ` +
      `The median right now is ${ms(trigger.baseline_ms)} ms, ` +
      `so the trigger fires past ${ms(Math.max(trigger.min_ms, trigger.baseline_ms * trigger.multiple))} ms.`
  );
  const pre = ImGui.InputInt("pre frames", trigger.pre);
  const post = ImGui.InputInt("post frames", trigger.post);
  if (pre.changed || post.changed) {
    setTrigger({ pre: Math.max(0, pre.value), post: Math.max(0, post.value) });
  }
  ImGui.SetItemTooltip(
    "`post` is not padding: a stutter's cause often shows in the recovery, so the " +
      "snapshot is deferred that many frames rather than taken as the trigger fires."
  );

  ImGui.SeparatorText("Captures");
  const captures = prof.captures;
  if (captures.length === 0) {
    ImGui.TextColored(DIM, "none yet");
  } else {
    const flags = ImGui.TableFlags.RowBg | ImGui.TableFlags.Borders;
    if (ImGui.BeginTable("prof-captures", 6, { flags })) {
      ImGui.TableSetupColumn("#");
      ImGui.TableSetupColumn("frame");
      ImGui.TableSetupColumn("ms");
      ImGui.TableSetupColumn("vs median");
      ImGui.TableSetupColumn("held");
      ImGui.TableSetupColumn("");
      ImGui.TableHeadersRow();
      for (const capture of captures) {
        ImGui.TableNextRow();
        ImGui.TableNextColumn();
        ImGui.Text(String(capture.index));
        ImGui.TableNextColumn();
        ImGui.Text(String(capture.frame_index));
        ImGui.TableNextColumn();
        if (capture.throttled) {
          ImGui.TextColored(AMBER, `${ms(capture.ms)} (vsync)`);
          ImGui.SetItemTooltip(
            "That frame was waiting for a blank anyway, so the trigger was measuring " +
              "the monitor - this is probably not the capture you wanted."
          );
        } else {
          ImGui.Text(ms(capture.ms));
        }
        ImGui.TableNextColumn();
        ImGui.Text(`${ms(capture.baseline_ms)}`);
        ImGui.TableNextColumn();
        if (capture.truncated) {
          ImGui.TextColored(RED, `${capture.frames}f (truncated)`);
          ImGui.SetItemTooltip(
            "Hit its fixed capacity and holds only part of the window. Raise " +
              "capture_events / capture_samples under Recording, or narrow pre/post."
          );
        } else {
          ImGui.Text(`${capture.frames}f ${capture.events}e ${capture.samples}s`);
        }
        ImGui.TableNextColumn();
        if (ImGui.SmallButton(`open##cap${capture.index}`)) {
          windowMode = "capture";
          captureIndex = capture.index;
          invalidate();
        }
      }
      ImGui.EndTable();
    }
    if (ImGui.Button("Clear captures")) {
      prof.clear_captures();
      invalidate();
    }
  }
  ImGui.TreePop();
}

/** @param {ImGui} ImGui */
function drawHealth(ImGui) {
  if (!ImGui.TreeNode("Threads and sampler")) {
    return;
  }
  ImGui.TextWrapped(
    "Whether the profile can be believed at all. A thread only appears once it has " +
      "recorded something."
  );

  const flags = ImGui.TableFlags.RowBg | ImGui.TableFlags.Borders;
  if (ImGui.BeginTable("prof-threads", 4, { flags })) {
    ImGui.TableSetupColumn("slot");
    ImGui.TableSetupColumn("thread");
    ImGui.TableSetupColumn("events");
    ImGui.TableSetupColumn("lost");
    ImGui.TableHeadersRow();
    for (const thread of prof.threads) {
      ImGui.TableNextRow();
      ImGui.TableNextColumn();
      ImGui.Text(String(thread.slot));
      ImGui.TableNextColumn();
      ImGui.Text(`${thread.name} (${thread.id})`);
      ImGui.TableNextColumn();
      ImGui.Text(String(thread.events));
      ImGui.TableNextColumn();
      if (thread.lost > 0) {
        ImGui.TextColored(RED, String(thread.lost));
        ImGui.SetItemTooltip(
          "Events overwritten before anything read them, so the window you are " +
            "asking for may be incomplete. Raise events_per_thread."
        );
      } else {
        ImGui.TextColored(DIM, "0");
      }
    }
    ImGui.EndTable();
  }

  const sampler = prof.sampler;
  ImGui.SeparatorText("Sampler");
  if (!sampler.running) {
    ImGui.TextColored(DIM, "not running - arm it with `sampler: true` under Recording");
  } else {
    // effective_hz, not hz: a flat profile only stands for time if the sampling
    // was uniform, and a periodic waitable timer does not hold 1 kHz under load.
    const ratio = sampler.hz > 0 ? sampler.effective_hz / sampler.hz : 0;
    ImGui.TextColored(
      ratio < 0.5 ? RED : ratio < 0.8 ? AMBER : GREEN,
      `${sampler.effective_hz.toFixed(0)} Hz effective of ${sampler.hz} asked ` +
        `(${pct(100 * ratio)}), drift ${ms(sampler.drift_ms)} ms`
    );
    ImGui.SetItemTooltip(
      "Read the effective rate, never the asked-for one. A flat profile is only " +
        "proportional to time if the sampling was uniform."
    );
    ImGui.Text(`${sampler.taken} taken, ${sampler.skipped} skipped, ${sampler.ticks} ticks`);
    if (sampler.missed > 0) {
      ImGui.TextColored(AMBER, `${sampler.missed} missed`);
      ImGui.SetItemTooltip(
        "A suspend or GetThreadContext the OS refused. A few are normal - a thread " +
          "exiting inside the window is one - but a rising count means the profile is " +
          "missing time rather than that there was none."
      );
    }
    if (sampler.walks > 0) {
      const barren = (100 * sampler.barren) / sampler.walks;
      ImGui.TextColored(
        barren > 30 ? AMBER : DIM,
        `${sampler.walks} stack walks, ${pct(barren)} barren`
      );
      ImGui.SetItemTooltip(
        "Walks that yielded no caller at all. A few percent is a sample landing in a " +
          "prologue; tens of percent against our own code is a defect."
      );
    }
  }

  const sites = prof.sites;
  if (ImGui.TreeNode(`Instrumented sites (${sites.length})`)) {
    ImGui.TextColored(DIM, "Every site whose code has run at least once, armed or not.");
    if (ImGui.BeginChild("prof-sites", { size: { x: 0, y: 160 } })) {
      for (const site of sites) {
        ImGui.Text(`${site.category.padEnd(7)} ${site.name}`);
      }
    }
    ImGui.EndChild();
    ImGui.TreePop();
  }
  ImGui.TreePop();
}

/** @param {ImGui} ImGui */
function drawRecording(ImGui) {
  if (!ImGui.TreeNode("Recording")) {
    return;
  }

  ImGui.SeparatorText("Categories");
  ImGui.TextWrapped(
    "Which categories record. `draw` is off by default: at ~60 ns a zone and ~700 " +
      "draws a frame it is 0.04 ms, which is 1% of a level frame and therefore " +
      "visible in what it measures."
  );
  const categories = prof.categories;
  const mask = prof.mask;
  let column = 0;
  for (const name of Object.keys(categories)) {
    if (column++ % 4 !== 0) {
      ImGui.SameLine();
    }
    const bit = categories[name];
    const on = ImGui.Checkbox(name, (mask & bit) !== 0);
    if (on.changed) {
      prof.mask = on.value ? mask | bit : mask & ~bit;
    }
  }

  ImGui.SeparatorText("Rings");
  ImGui.TextWrapped(
    "Applied by the button, not by the widget: configure() re-arms, and a ring only " +
      "ever grows - the profiler never shrinks or frees one, because a recorder on " +
      "the other thread could be inside it."
  );
  configInt(ImGui, "events_per_thread");
  configInt(ImGui, "frames");
  configInt(ImGui, "samples");
  configInt(ImGui, "sampler_hz");
  configBool(ImGui, "sampler");
  configBool(ImGui, "stacks");
  configInt(ImGui, "stack_depth");
  configInt(ImGui, "captures");
  configInt(ImGui, "capture_events");
  configInt(ImGui, "capture_samples");
  if (ImGui.Button("Apply")) {
    try {
      prof.configure(pendingConfig);
      configNote = "re-armed";
    } catch (error) {
      configNote = String(error);
    }
    pendingConfig = {};
    invalidate();
  }
  ImGui.SameLine();
  if (ImGui.Button("Discard")) {
    pendingConfig = {};
    configNote = null;
  }
  ImGui.SameLine();
  if (ImGui.Button("Reset rings")) {
    prof.reset();
    history = [];
    throttledHistory = [];
    lastFrameIndex = -1;
    invalidate();
  }
  ImGui.SetItemTooltip("Empties the rings and the frame history. Does not disarm.");
  ImGui.SameLine();
  if (ImGui.Button("Disarm")) {
    prof.enabled = false;
    invalidate();
  }
  if (configNote) {
    ImGui.TextColored(DIM, configNote);
  }

  ImGui.SeparatorText("Export");
  const path = ImGui.InputText("trace path", tracePath);
  if (path.changed) {
    tracePath = path.text;
  }
  if (ImGui.Button("Write trace")) {
    try {
      note = `wrote ${prof.trace(tracePath, queryWindow())} (${windowLabel()})`;
    } catch (error) {
      note = String(error);
    }
  }
  ImGui.SetItemTooltip(
    "A Chrome-trace / Perfetto document over the CURRENT window - one track per " +
      "thread, samples as instants, frames on their own track. chrome://tracing " +
      "or ui.perfetto.dev."
  );

  ImGui.SeparatorText("Symbols");
  ImGui.TextWrapped(
    `Looked for automatically in ${prof.symbol_dir} as <module>.sym, so installing ` +
      `one is all it takes. utils/symdump/gl_symbols.py exports gl.exe's from Ghidra.`
  );
  const mod = ImGui.InputText("module", symbolModule);
  if (mod.changed) {
    symbolModule = mod.text;
  }
  const sym = ImGui.InputText("map path", symbolPath);
  if (sym.changed) {
    symbolPath = sym.text;
  }
  if (ImGui.Button("Load symbols")) {
    try {
      const loaded = prof.symbols(symbolModule, symbolPath);
      note =
        `${loaded.entries} symbols for ${symbolModule}` +
        (loaded.stale ? ` - STALE: ${loaded.note}` : "");
      invalidate();
    } catch (error) {
      note = String(error);
    }
  }
  ImGui.TreePop();
}

// --- helpers -----------------------------------------------------------------

/** Slot index -> thread name, for the tables. One read a frame of a short array;
 *  the rows themselves carry only the slot.
 *  @returns {Record<number, string>} */
function threadNames() {
  /** @type {Record<number, string>} */
  const names = {};
  for (const thread of prof.threads) {
    names[thread.slot] = thread.name;
  }
  return names;
}

/** @param {ImGui} ImGui @param {keyof import("gk").ProfConfig} key */
function configInt(ImGui, key) {
  const result = ImGui.InputInt(key, Number(pending(key)));
  if (result.changed) {
    pendingConfig[key] = Math.max(0, result.value);
  }
  if (pendingConfig[key] !== undefined) {
    ImGui.SameLine();
    ImGui.TextColored(AMBER, "*");
  }
}

/** @param {ImGui} ImGui @param {keyof import("gk").ProfConfig} key */
function configBool(ImGui, key) {
  const result = ImGui.Checkbox(key, pending(key) === true);
  if (result.changed) {
    pendingConfig[key] = result.value;
  }
  if (pendingConfig[key] !== undefined) {
    ImGui.SameLine();
    ImGui.TextColored(AMBER, "*");
  }
}
