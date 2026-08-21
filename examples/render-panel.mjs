// Every `render` knob worth turning, as ImGui.
//
// Its own module rather than more of main.mjs, for two reasons: main.mjs is a
// teaching example and this would swamp it, and a panel you can copy into your
// own entry module beside your own code is more use than one you have to cut out
// of somebody else's.
//
// It draws headers into whatever window the caller has open - no Begin/End of its
// own - so `draw_render_panel(ImGui)` composes with anything.
//
// ## One rule, and it is not a style preference
//
// **Write a knob only when the widget says it changed.** `draw_gui` runs every
// frame the overlay is open, and several of these setters do real work:
// `render.lighting_maps = true` destroys every lighting-map image and re-reads
// every file, `render.map_shadow_rate` re-bakes the shadow atlas from the start,
// and `render.map_shadow_indirect` rebuilds a pipeline. Some of them happen to
// early-out when the value is unchanged; writing them unconditionally would be
// betting on which ones, sixty times a second. So everything here goes through
// the three helpers below, which write on `changed` and nothing else.
//
// The second cost is reading. `render.draws`, `render.vulkan_report` and the rest
// format a page of text every time they are touched, so nothing here reads one
// per frame - they sit behind a button that caches what it read.

import { console, render } from "gk";

/** Text the readout buttons have fetched, so a page of formatted state is built
 *  when it is asked for rather than sixty times a second.
 *  @type {{title: string, body: string} | null} */
let readout = null;

/** The sample counts offered, and their labels. Not read from the device - a
 *  count it cannot do rounds down on the way in, and `render.status` says so. */
const MSAA_COUNTS = [1, 2, 4, 8];
const MSAA_LABELS = ["off", "2x", "4x", "8x"];

/** The operator names, in the order the combo lists them.
 *  Annotated rather than inferred: without it the element type is `string`, and
 *  `render.tonemap` is a union of the six literals - so the assignment below is
 *  exactly what the type check catches.
 *  @type {Array<"clamp" | "rolloff" | "reinhard" | "aces" | "filmic" | "agx">} */
const TONEMAP_OPS = ["clamp", "rolloff", "reinhard", "aces", "filmic", "agx"];

/** The blend names, in the order the per-layer combo lists them. Annotated for
 *  the same reason TONEMAP_OPS is: without it the element type is `string`, and
 *  `BloomLayerSpec.blend` is a union of the four literals.
 *  @type {Array<"off" | "add" | "screen" | "max">} */
const BLOOM_BLENDS = ["off", "add", "screen", "max"];

/** The count asked for while the frame that adopts it has not run yet, or null.
 *  `render.msaa` reads back what is IN FORCE, so a control bound straight to it
 *  would show the old value for one frame and write it back over the request.
 *  @type {number | null} */
let msaaPending = null;

/** A checkbox bound to a `render` property, written only when it changes.
 *  @param {ImGui} ImGui
 *  @param {string} label
 *  @param {string} key
 *  @param {string} [tip]
 *  @returns {boolean} the value now, so a caller can grey out what depends on it
 */
function toggle(ImGui, label, key, tip) {
  const result = ImGui.Checkbox(label, render[key] === true);
  if (result.changed) {
    render[key] = result.value;
  }
  if (tip) {
    ImGui.SetItemTooltip(tip);
  }
  return result.value;
}

/** A float slider bound to a `render` property, written only when it changes.
 *  @param {ImGui} ImGui
 *  @param {string} label
 *  @param {string} key
 *  @param {number} min
 *  @param {number} max
 *  @param {string} [tip]
 *  @param {string} [format]
 */
function slider(ImGui, label, key, min, max, tip, format) {
  const current = Number(render[key]);
  const result = ImGui.SliderFloat(label, Number.isFinite(current) ? current : 0, min, max, {
    format: format ?? "%.3f",
  });
  if (result.changed) {
    render[key] = result.value;
  }
  if (tip) {
    ImGui.SetItemTooltip(tip);
  }
}

/** The same for a whole-number knob.
 *  @param {ImGui} ImGui
 *  @param {string} label
 *  @param {string} key
 *  @param {number} min
 *  @param {number} max
 *  @param {string} [tip]
 */
function intSlider(ImGui, label, key, min, max, tip) {
  const current = Number(render[key]);
  const result = ImGui.SliderInt(label, Number.isFinite(current) ? current : min, min, max);
  if (result.changed) {
    render[key] = result.value;
  }
  if (tip) {
    ImGui.SetItemTooltip(tip);
  }
}

/** A button that fetches one of the text readouts into the cache.
 *  @param {ImGui} ImGui
 *  @param {string} label
 *  @param {() => string} read
 */
function readoutButton(ImGui, label, read) {
  if (ImGui.Button(label)) {
    try {
      readout = { title: label, body: read() };
    } catch (error) {
      readout = { title: label, body: String(error) };
    }
  }
}

/**
 * Draw the whole panel into the caller's window.
 *
 * @param {ImGui} ImGui
 */
export function draw_render_panel(ImGui) {
  if (!ImGui.CollapsingHeader("Renderer")) {
    return;
  }
  ImGui.TextWrapped(
    "These are the Vulkan renderer's own knobs. Most are A/B switches meant to be " +
      "toggled on a paused frame (screen.toggle_pause) - that is the only comparison " +
      "with a zero noise floor."
  );

  // Narrow enough that a label fits beside it in the default overlay width.
  ImGui.PushItemWidth(150);

  // --- the whole departure set, at the top because it moves everything below --
  //
  // Not inside a TreeNode: it is the one control here that answers a question
  // ("what did the original look like?") rather than tuning an answer, and the
  // ten switches it drives are scattered across seven of the sections below.
  toggle(ImGui, "stock D3D8 look", "stock",
    "Every deliberate departure off at once - per-pixel lighting, map lighting, " +
      "lighting maps, all four shadow systems, AO, tessellation and MSAA. The setup " +
      "for an A/B against GKPLUS_RENDERER=d3d8. Off restores what you had, not the " +
      "defaults, and leaves every slider alone.");
  ImGui.Separator();

  // --- the sun's shadow ------------------------------------------------------
  if (ImGui.TreeNode("Sun shadow")) {
    const sun = toggle(ImGui, "sun_shadows", "sun_shadows",
      "A real shadow map from the sun. Off is the build before it existed.");
    // Everything below only does something while the sun is casting, and a knob
    // that visibly does nothing is worse than one that is visibly unavailable.
    ImGui.BeginDisabled(!sun);
    intSlider(ImGui, "cascades", "shadow_cascades", 1, 4,
      "1 is the original single map at the same texel density - the A/B for cascading.");
    slider(ImGui, "bias (texels)", "shadow_bias", 0, 8,
      "In shadow texels, so one value holds on every cascade, level and extent. " +
        "Below ~2 the level shadows itself; above ~2.5 the shadow shrinks away from its caster.",
      "%.2f");
    slider(ImGui, "strength", "shadow_strength", 0, 1,
      "1 is 'no sunlight reaches here', which is correct rather than maximum - " +
        "ambient and the level's own bake still light the surface.",
      "%.2f");
    slider(ImGui, "extent", "shadow_extent", 10, 250,
      "Half-width of the OUTERMOST cascade, in world units. The camera's own max " +
        "distance is 75, so 70 covers everything it can see.",
      "%.0f");
    ImGui.EndDisabled();
    toggle(ImGui, "stencil_shadow", "stencil_shadow",
      "Also draw the game's own blob shadow. Off, or a unit carries both.");
    ImGui.TreePop();
  }

  // --- the level's own light rig ---------------------------------------------
  if (ImGui.TreeNode("Map lighting")) {
    const on = toggle(ImGui, "map_lighting", "map_lighting",
      "Replace the level's baked vertex colour with a per-pixel evaluation of the " +
        "rig that baked it. Judge it on level04 or level05, not level02.");
    ImGui.BeginDisabled(!on);
    slider(ImGui, "gain", "map_light_gain", 0.5, 2.5,
      "The fitted model's one free parameter. 0.9 on level01, 1.35 on level04 and 05.",
      "%.2f");
    toggle(ImGui, "cull by the grid", "map_light_cull",
      "Bin the lights into a world-space grid. Worth 28 ms a frame on level01 - and " +
        "off must be BIT-IDENTICAL, which is the only test that catches a cell missing a light.");
    toggle(ImGui, "substitute everywhere", "map_lighting_all",
      "Also substitute on props and units, which carry their own file's bake. " +
        "Measures worse; it exists so that claim stays checkable.");

    ImGui.SeparatorText("Static shadows from those lights");
    const shadows = toggle(ImGui, "map_shadows", "map_shadows",
      "One six-face cube per light, baked once per level. The bake is gated on this " +
        "too, so off costs nothing and turning it back on restarts it.");
    ImGui.BeginDisabled(!shadows);
    slider(ImGui, "normal offset", "map_shadow_bias", 0, 4,
      "In atlas texels at the fragment's distance. 0 shows exactly what the atlas " +
        "holds: per-light acne, one colour per light.",
      "%.2f");
    intSlider(ImGui, "bake rate", "map_shadow_rate", 1, 512,
      "Lights baked per frame. Writing this re-bakes from the start.");
    toggle(ImGui, "indirect submission", "map_shadow_indirect",
      "One vkCmdDrawIndexedIndirect a face instead of a draw call per caster per " +
        "face. The two must produce the same atlas - this is what checks it.");
    ImGui.EndDisabled();
    readoutButton(ImGui, "map_shadow_report", () => String(render.map_shadow_report));
    ImGui.SameLine();
    readoutButton(ImGui, "map_light_report", () => String(render.map_light_report));
    ImGui.EndDisabled();
    ImGui.TreePop();
  }

  // --- ambient occlusion -----------------------------------------------------
  //
  // Its own section rather than a line under a light system, because it belongs to
  // none of them: it is a screen-space term that scales whichever sum ran.
  if (ImGui.TreeNode("Ambient occlusion")) {
    const ao = toggle(ImGui, "ao", "ao",
      "Screen-space AO with NO blur pass - one fixed LATTICE disc shared by every " +
        "pixel, so the output is not noise. Off by default: the game never had it.");
    ImGui.BeginDisabled(!ao);
    toggle(ImGui, "debug view", "ao_debug",
      "Show the occlusion term itself as grey. Tune the radius and the tap count " +
        "against THIS - both are close to invisible in a shaded frame.");
    slider(ImGui, "radius (world)", "ao_radius", 0.1, 8,
      "The hemisphere, in world units - what 'near enough to occlude' means. 3 is a " +
        "sweep: past it the disc binds instead and only the over-darkening grows.",
      "%.2f");
    slider(ImGui, "radius (of height)", "ao_screen_radius", 0.002, 0.12,
      "The disc, as a fraction of the frame's height, and independent of the one " +
        "above. Not pixels: the render extent is 640x480 on one machine and " +
        "3072x1728 on another. 0.07 is 34 px at 480 lines, 121 at 1728.",
      "%.4f");
    intSlider(ImGui, "taps", "ao_taps", 1, 64,
      "How much of the 64-point disc to walk - all of it by default. Lowering it " +
        "does not add grain, it adds a visible copy of every silhouette per missing " +
        "tap. 32 is exactly the pattern the technique's author published; the set is " +
        "maximin-ordered, so any lower count is still well spread.");
    slider(ImGui, "bias", "ao_bias", 0, 0.5,
      "How far along the normal a tap must be to count, in world units. Too low and " +
        "a flat wall shades itself; too high and a shallow crease vanishes.",
      "%.3f");
    slider(ImGui, "strength", "ao_strength", 0, 2,
      "A scale on the occlusion before it leaves the resolve pass.",
      "%.2f");
    slider(ImGui, "direct", "ao_direct", 0, 1,
      "How much it also scales D3D's OWN diffuse sum - the sun and the dynamic " +
        "lights, not the map rig, which is always occluded in full. 0 because each " +
        "of those already has a shadow map. The specular is never occluded.",
      "%.2f");
    toggle(ImGui, "map geometry only", "ao_map_only",
      "On. A prop or a unit carries a bake that already contains occlusion, so " +
        "applying this there darkens the same crease twice.");
    ImGui.EndDisabled();
    ImGui.TreePop();
  }

  // --- the game's own D3D point and spot lights ------------------------------
  //
  // Deliberately its own section rather than a line in "Map lighting": they share
  // an atlas and nothing else. These are the lights the game sets on the device
  // this frame - level02's fires - where the map lights are authoring data the
  // shipped engine loaded and never read.
  if (ImGui.TreeNode("Point and spot lights")) {
    toggle(ImGui, "local_shadows", "local_shadows",
      "Shadows from the game's own point and spot lights, out of sixteen reserved " +
        "slots of the map lights' atlas. A light that MOVES gets none rather than a " +
        "wrong one, and costs nothing.");
    toggle(ImGui, "local_lights", "local_lights",
      "Whether those lights are in the sum at all. A diagnostic: off paints exactly " +
        "the pixels they reach, which is the ceiling on what shadowing them could change.");
    readoutButton(ImGui, "local_shadow_report", () => String(render.local_shadow_report));
    ImGui.SameLine();
    readoutButton(ImGui, "frame_lights", () => String(render.frame_lights));
    ImGui.TreePop();
  }

  // --- the light sum ---------------------------------------------------------
  if (ImGui.TreeNode("Light sum")) {
    toggle(ImGui, "per_pixel_lighting", "per_pixel_lighting",
      "D3D8's light sum per fragment rather than per vertex. Same equation, same " +
        "lights. Judge it on the difference image: it is zero on flat ground.");
    toggle(ImGui, "lighting", "lighting",
      "The whole light sum. Off collapses to the material colour, which is what " +
        "the build before it did.");
    toggle(ImGui, "specular", "specular",
      "The specular term of that sum. The run-time mirror of GKPLUS_NO_SPECULAR, " +
        "which reaches only the forwarded call.");
    toggle(ImGui, "shade_mode", "shade_mode",
      "Honour D3DRS_SHADEMODE. Worth 0 pixels on level01 and level02, where every " +
        "flat-shaded draw is the stencil shadow.");
    ImGui.TreePop();
  }

  // --- lighting maps ---------------------------------------------------------
  if (ImGui.TreeNode("Lighting maps")) {
    const maps = toggle(ImGui, "lighting_maps", "lighting_maps",
      "Load '<texture> lighting.dds' beside each .RIM. Setting this false then " +
        "true re-reads every file - that is the authoring gesture for a map edited " +
        "while the game runs.");
    ImGui.BeginDisabled(!maps);
    slider(ImGui, "bump_scale", "bump_scale", 0, 4, undefined, "%.2f");
    slider(ImGui, "bump_diffuse", "bump_diffuse", 0, 1,
      "A bump that only shapes highlights is invisible wherever metallic is 0, " +
        "which is why the derived normal reaches the diffuse too.", "%.2f");
    slider(ImGui, "bump_diffuse_limit", "bump_diffuse_limit", 1, 8,
      "How far the diffuse ratio may carry a pixel either way. 4 is the old " +
        "ceiling; the floor stops a surface near a light's terminator going black.",
      "%.2f");
    slider(ImGui, "specular_scale", "specular_scale", 0, 1,
      "0.25 because level02's key light is diffuse 4.0 and 1.0 saturates a floor to white.",
      "%.2f");
    slider(ImGui, "specular_from_diffuse", "specular_from_diffuse", 0, 1,
      "1 because every light reaching that floor authors specular 0 0 0, so at 0 " +
        "the metallic channel would do nothing over most of a level.", "%.2f");
    slider(ImGui, "gloss_min", "gloss_min", 1, 128, undefined, "%.0f");
    slider(ImGui, "gloss_max", "gloss_max", 1, 512, undefined, "%.0f");
    slider(ImGui, "chrome_scale", "chrome_scale", 0, 2, undefined, "%.2f");
    slider(ImGui, "chrome_blur", "chrome_blur", 0, 8,
      "A mip bias on the sphere map. The game's own sampler clamps to level 0, so " +
        "this swaps in a sampler that does not.", "%.2f");
    toggle(ImGui, "chrome_texgen", "chrome_texgen");
    readoutButton(ImGui, "lighting_map_report", () => String(render.lighting_map_report));
    ImGui.SetItemTooltip(
      "Not optional reading: a texture with no companion file is the normal case, " +
        "so a misnamed file and a stock install look identical from the screen."
    );
    ImGui.EndDisabled();
    ImGui.TreePop();
  }

  // --- PN-triangle amplification ---------------------------------------------
  if (ImGui.TreeNode("Tessellation")) {
    ImGui.TextWrapped(
      "PN triangles over the level mesh. A corner whose normal is its face normal " +
        "contributes nothing to the patch, so hard edges stay hard by arithmetic " +
        "rather than by a heuristic - and a boulder or a pipe rounds off."
    );
    const on = toggle(ImGui, "tessellation", "tessellation",
      "Off by default: it changes the level's silhouette rather than reproducing D3D. " +
        "Reads back false on a device with no tessellationShader whatever you set.");
    ImGui.BeginDisabled(!on);
    // A combo would be tidier; three buttons keep this to the write-on-changed rule
    // without a selection index to track.
    /** @type {Array<"map" | "all" | "off">} */
    const sets = ["map", "all", "off"];
    for (const set of sets) {
      if (set !== "map") {
        ImGui.SameLine();
      }
      if (ImGui.RadioButton(set, render.tess_set === set) && render.tess_set !== set) {
        render.tess_set = set;
      }
    }
    ImGui.SetItemTooltip(
      "Which draws are amplified. 'all' is not a debug setting: render.normal_census() " +
        "measures more than half a frame's curvature outside the map object."
    );
    slider(ImGui, "tess_edge_pixels", "tess_edge_pixels", 4, 128,
      "The screen-space edge length a factor aims for.", "%.0f");
    slider(ImGui, "tess_max", "tess_max", 1, 32,
      "Clamped to the device's maxTessellationGenerationLevel.", "%.0f");
    slider(ImGui, "tess_min", "tess_min", 1, 32,
      "Above 1 this forces uniform amplification, which is how the shape can be " +
        "judged without the factors varying underneath it.", "%.0f");
    slider(ImGui, "pn_strength", "pn_strength", 0, 1,
      "0 is exactly linear - the untessellated surface however high the factors go. " +
        "That is the A/B separating 'the amplification is wrong' from 'the curve is'.",
      "%.2f");
    slider(ImGui, "pn_flat_threshold", "pn_flat_threshold", 0, 0.2,
      "Snap a near-flat corner to exactly flat. Only 6.4% of level02's map triangles " +
        "are fully flat on their own, so this is what keeps walls from doming.",
      "%.3f");
    toggle(ImGui, "pn_seam_fix", "pn_seam_fix",
      "Zero the tangent term where the mesh has split a corner into two differently-" +
        "normalled vertices, which is what keeps a material boundary from tearing open. " +
        "Off reproduces the tear; seam_census below is the check.");
    toggle(ImGui, "tess_shadows", "tess_shadows",
      "Amplify in the shadow passes too. Separable because the bake is where the " +
        "cost is - and re-baking is what makes a change here visible in the atlas.");
    slider(ImGui, "tess_shadow_factor", "tess_shadow_factor", 1, 16,
      "Uniform over every edge, which makes those passes watertight for free.",
      "%.0f");
    readoutButton(ImGui, "normal_census", () => render.normal_census());
    ImGui.SetItemTooltip(
      "How much of the frame carries smooth normals at all - which is the ceiling on " +
        "what this feature can reach. Reads the arena back; do not call it per frame."
    );
    readoutButton(ImGui, "seam_census", () => render.seam_census());
    ImGui.SetItemTooltip(
      "Where two triangles meet, do their two patches agree? A corner the mesh has split " +
        "into two differently-normalled vertices tears open, and this prices it in world " +
        "units at the knobs above. Reads the arena back; do not call it per frame."
    );
    ImGui.EndDisabled();
    ImGui.TreePop();
  }

  // --- multisampling ---------------------------------------------------------
  if (ImGui.TreeNode("Antialiasing")) {
    // The one control in this file that does NOT read the knob straight back,
    // and the module-level `msaaPending` above is why. `render.msaa` answers
    // with the count in force, and the frame that adopts a new one has not
    // started yet - so a Combo bound to the raw getter would show the old value
    // for a frame, decide it "changed" back, and write the old value over the
    // new one. Holding the request until the getter agrees is the whole fix.
    const live = Number(render.msaa);
    if (msaaPending !== null && msaaPending === live) {
      msaaPending = null;
    }
    const shown = msaaPending ?? live;
    const index = Math.max(0, MSAA_COUNTS.indexOf(shown));
    const picked = ImGui.Combo("samples", index, MSAA_LABELS);
    if (picked.changed) {
      msaaPending = MSAA_COUNTS[picked.current_item];
      render.msaa = msaaPending;
    }
    ImGui.SetItemTooltip(
      "Sample count for the world pass. Takes effect next frame - it rebuilds the " +
        "target, the depth buffer and every cached pipeline. Smooths geometric edges " +
        "and the stencil shadow's border; does nothing for alpha-tested cutouts."
    );
    if (msaaPending !== null) {
      ImGui.TextWrapped(`waiting for the next frame (in force: ${live}x)`);
    }
    ImGui.TreePop();
  }

  // --- HDR -------------------------------------------------------------------
  //
  // No pending value here, unlike the antialiasing above: `render.hdr` reads back
  // as REQUESTED rather than as in force, precisely so a control can bind to it
  // directly. What is not in force yet shows up in `render.draws` in words.
  if (ImGui.TreeNode("HDR")) {
    const hdr = toggle(ImGui, "hdr", "hdr",
      "Render into R16G16B16A16_SFLOAT and tonemap to the swapchain. Linear-light " +
        "lighting and no D3DCOLOR clamp - worth 9.56/255 over 83% of level02, most " +
        "of it shadow detail. Off is the shipped renderer.");
    ImGui.BeginDisabled(!hdr);
    toggle(ImGui, "linear_input", "linear_input",
      "Decode the fragment's final colour so the blend and the tonemap run on light. " +
        "The shading itself is untouched - additive fires and flares accumulate past 1 " +
        "instead of clipping, and an opaque draw is bit-exact against stock.");
    const op = render.tonemap;
    const picked = ImGui.Combo("tonemap", Math.max(0, TONEMAP_OPS.indexOf(op)), TONEMAP_OPS);
    if (picked.changed) {
      render.tonemap = TONEMAP_OPS[picked.current_item];
    }
    ImGui.SetItemTooltip(
      "Applies to the world alone - the menus, HUD and briefing screens are drawn after " +
        "the tonemap, so no operator reaches them. rolloff is the default because it is " +
        "the identity below the knee and touches only what exceeds it."
    );
    slider(ImGui, "exposure", "exposure", 0.1, 4.0,
      "A linear multiplier applied BEFORE the operator, which is the only place it " +
        "can go.");
    slider(ImGui, "knee", "tonemap_knee", 0.0, 1.0,
      "Where rolloff stops being the identity. Read by that operator alone.");
    slider(ImGui, "white", "tonemap_white", 1.0, 16.0,
      "What reinhard maps to exactly 1.0. Read by that operator alone.", "%.1f");
    ImGui.EndDisabled();
    ImGui.TreePop();
  }

  // --- bloom -----------------------------------------------------------------
  //
  // Nested under its own node rather than inside the HDR one, but disabled by the
  // same flag: bloom is inert without a float target, and a control that silently
  // does nothing is worse than a greyed-out one.
  //
  // **`render.bloom_layer` is a function, so the write-on-changed rule needs
  // stating again here rather than being carried by the helpers.** Each layer is
  // read once per frame - which is cheap, five numbers out of a struct - and
  // written only from a widget's own `changed`, one field at a time, because the
  // spec is partial. Writing all five back on every frame would work and would
  // also mean a slider you are dragging fights any other writer of the same
  // layer.
  if (ImGui.TreeNode("Bloom")) {
    const bloomHdr = render.hdr === true;
    const bloom = toggle(ImGui, "bloom", "bloom",
      "Extract the over-range part of the world image, blur it at three scales, and " +
        "add it back inside the tonemap pass. Needs hdr: an 8-bit target has nothing " +
        "over 1 in it to threshold.");
    if (!bloomHdr) {
      ImGui.TextWrapped("hdr is off, so this is inert - there is no over-range light to find.");
    }
    ImGui.BeginDisabled(!bloom || !bloomHdr);
    for (let index = 0; index < 3; index += 1) {
      const layer = render.bloom_layer(index);
      const width = ["tight", "mid", "wide"][index];
      // **`##${index}` on the node's label, and the five widgets inside need no
      // suffix of their own.** An open TreeNode pushes its own id, so its
      // children are already scoped per layer - which is what saves three
      // identical `threshold` sliders from being one widget. The suffix is on
      // this label because everything before it is *display* text that changes
      // when the blend does, and an id that moves collapses the node the moment
      // you use it.
      if (ImGui.TreeNode(`layer ${index} - ${width} (${layer.blend})##${index}`)) {
        const blend = ImGui.Combo("blend", Math.max(0, BLOOM_BLENDS.indexOf(layer.blend)),
          BLOOM_BLENDS);
        if (blend.changed) {
          render.bloom_layer(index, { blend: BLOOM_BLENDS[blend.current_item] });
        }
        ImGui.SetItemTooltip(
          "off skips the layer's two GPU passes entirely. add is what a lens does; " +
            "screen puts the glow only where the frame is not already lit, which is what " +
            "keeps a wide layer from reading as fog; max cannot brighten a bright area."
        );
        const threshold = ImGui.SliderFloat("threshold", layer.threshold, 0.0, 4.0,
          { format: "%.2f" });
        if (threshold.changed) {
          render.bloom_layer(index, { threshold: threshold.value });
        }
        ImGui.SetItemTooltip(
          "Linear luminance at which a pixel starts to contribute. 1.0 is exactly a " +
            "fully lit opaque surface, so at or above it only over-range light glows."
        );
        const knee = ImGui.SliderFloat("knee", layer.knee, 0.0, 2.0, { format: "%.2f" });
        if (knee.changed) {
          render.bloom_layer(index, { knee: knee.value });
        }
        ImGui.SetItemTooltip(
          "Half-width of the soft ramp around the threshold. Zero pops a surface's whole " +
            "contribution on and off between two frames as the camera drifts."
        );
        const radius = ImGui.SliderFloat("radius", layer.radius, 0.0, 0.15,
          { format: "%.4f" });
        if (radius.changed) {
          render.bloom_layer(index, { radius: radius.value });
        }
        ImGui.SetItemTooltip(
          "The blur's sigma as a fraction of the FRAME HEIGHT, so it means the same at " +
            "any resolution. render.bloom_layers says what it works out to in texels."
        );
        const intensity = ImGui.SliderFloat("intensity", layer.intensity, 0.0, 2.0,
          { format: "%.2f" });
        if (intensity.changed) {
          render.bloom_layer(index, { intensity: intensity.value });
        }
        ImGui.SetItemTooltip(
          "The multiplier at composite time. Zero still records both passes - use " +
            "blend: off to switch a layer off for free."
        );
        ImGui.TreePop();
      }
    }
    ImGui.EndDisabled();
    readoutButton(ImGui, "bloom layers", () => render.bloom_layers);
    ImGui.SetItemTooltip(
      "Each layer's size, the sigma its radius works out to in that layer's own texels, " +
        "and whether the pass built on this device."
    );
    ImGui.TreePop();
  }

  // --- fidelity against the original -----------------------------------------
  if (ImGui.TreeNode("Fidelity switches")) {
    ImGui.TextWrapped(
      "Each of these reproduces something D3D8 does. Turning one off is how it was " +
        "measured; leaving one off is a renderer that no longer matches."
    );
    toggle(ImGui, "offscreen", "offscreen",
      "Rasterise at the game's 640x480 backbuffer size and blit, rather than " +
        "letting the viewport scale every 2D draw. Worth 2.55/255 over 65% of the frame.");
    toggle(ImGui, "half_pixel", "half_pixel",
      "D3D9's pixel-centre convention, as a half-pixel viewport origin. 1.34/255.");
    toggle(ImGui, "rhw_depth_raw", "rhw_depth_raw",
      "A pre-transformed vertex's z is clamped into the viewport slice, not run " +
        "through it. Off makes level02's flames come and go with camera distance.");
    toggle(ImGui, "viewport_rect", "viewport_rect",
      "Honour D3DVIEWPORT8's rectangle per draw. Only the upgrade screen sets one " +
        "that is not the whole backbuffer.");
    toggle(ImGui, "present_linear", "present_linear",
      "Filter the final blit. NEAREST is a deduction, not a default: the original's " +
        "stretch preserves a 4-bit texture's sixteen distinct values.");
    ImGui.TreePop();
  }

  // --- diagnostics -----------------------------------------------------------
  if (ImGui.TreeNode("Diagnostics")) {
    slider(ImGui, "force_lod", "force_lod", -1, 12,
      "Force every texture fetch to one mip level. -1 is off. Pair it with " +
        "GKPLUS_NO_MIPMAP=1 on the reference to pin both sides to level 0.",
      "%.0f");

    // The bisect. `draw_hide` is the one to reach for: hiding a window leaves the
    // depth and stencil buffers intact, where truncating a prefix does not - and a
    // draw that is merely unoccluded then reads as the one that painted the pixel.
    //
    // **`draw_hide` always reads back as a two-element array**, and "nothing
    // hidden" is `[1, 0]` - a window no index can fall in. So the empty state is
    // `first > last`, not null and not an empty array: testing the array for
    // truthiness says "something is hidden" every single frame.
    ImGui.SeparatorText("Bisect the draw list");
    const window = /** @type {number[]} */ (render.draw_hide);
    const from = window[0] ?? 1;
    const to = window[1] ?? 0;
    ImGui.Text(from > to ? "hiding nothing" : `hiding draws ${from}..${to}`);
    // Typed rather than dragged: a bisect converges on one index, and a slider
    // over a frame's ~300 draws cannot address one.
    const first = ImGui.InputInt("hide from", from);
    const last = ImGui.InputInt("hide to", to);
    if (first.changed || last.changed) {
      render.draw_hide = [first.value, last.value];
    }
    if (ImGui.Button("Show everything")) {
      render.draw_hide = null;
      render.draw_range = null;
    }
    ImGui.SetItemTooltip("Clears both draw_hide and draw_range.");

    ImGui.SeparatorText("Readouts");
    readoutButton(ImGui, "draws", () => String(render.draws));
    ImGui.SameLine();
    readoutButton(ImGui, "vulkan_report", () => String(render.vulkan_report));
    ImGui.SameLine();
    readoutButton(ImGui, "validation", () => JSON.stringify(render.validation, null, 1));
    readoutButton(ImGui, "verify_textures()", () => String(render.verify_textures()));
    ImGui.SameLine();
    readoutButton(ImGui, "verify_buffers()", () => String(render.verify_buffers()));
    ImGui.SetItemTooltip(
      "Reads one short on a running level and that is the instrument, not a defect: " +
        "a dynamic buffer the game refills while the verifier reads it. Pause first."
    );
    if (ImGui.Button("Reset counters")) {
      render.reset();
    }
    ImGui.TreePop();
  }

  // --- material overrides ----------------------------------------------------
  if (ImGui.TreeNode("Material override")) {
    ImGui.TextWrapped(
      "Names a case-insensitive substring of a live texture's .rim path. An override " +
        "that resolves and paints nothing looks exactly like a broken one - the " +
        "readback is what tells them apart."
    );
    const name = ImGui.InputText("texture", overrideName);
    if (name.changed) {
      overrideName = name.text;
    }
    if (ImGui.Button("Tint magenta")) {
      readout = {
        title: "material_override",
        body: render.material_override(overrideName, { tint: [1, 0, 1] }),
      };
    }
    ImGui.SameLine();
    if (ImGui.Button("Hide")) {
      readout = {
        title: "material_override",
        body: render.material_override(overrideName, { hide: true }),
      };
    }
    ImGui.SameLine();
    if (ImGui.Button("Clear all")) {
      render.clear_material_overrides();
      readout = { title: "material_override", body: "cleared" };
    }
    readoutButton(ImGui, "material_overrides", () => String(render.material_overrides));
    ImGui.TreePop();
  }

  ImGui.PopItemWidth();

  // Whatever a readout button last fetched, in a scrolling child so a page of
  // text does not push the rest of the overlay off screen.
  // Copied into a local first: the Close button below clears the module-level
  // one, and reading it again afterwards would be reading what was just dropped.
  const shown = readout;
  if (shown) {
    ImGui.SeparatorText(shown.title);
    if (ImGui.Button("Close")) {
      readout = null;
    }
    if (ImGui.BeginChild("render-readout", { size: { x: 0, y: 220 } })) {
      ImGui.TextWrapped(shown.body);
    }
    ImGui.EndChild();
  }
}

/** The material-override key, kept across frames because InputText hands back
 *  the edited text rather than writing through a pointer.
 *  @type {string} */
let overrideName = "";

/** Log the state of everything this panel touches, for a bug report or a note.
 *  Exported because it is the one thing more useful from the console than from
 *  a slider. */
export function log_render_state() {
  console.log(String(render.draws));
  console.log(String(render.vulkan_report));
}
