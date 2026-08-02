// The `render` namespace. Today it is the Phase 0b measurement surface for the Vulkan
// renderer (vulkan_renderer_notes.md section 4) and nothing else - it reads the counters
// src/D3D8Capture.cpp accumulates and can clear them. The eventual shape (post-process pass
// registration, material override, draw-list introspection) is section 5 of that file.
//
// Everything here is read-only against the game: the capture layer forwards every call
// unchanged, so nothing in this namespace can alter what is drawn.

#include "D3D8Capture.h"
#include "JsBindings.h"
#include "VkCapture.h"
#include "VkContext.h"
#include "VkDraw.h"
#include "VkRenderer.h"
#include "VkResources.h"

#include <iterator>

namespace gk::js {
namespace {

// A std::map<uint32_t, T> as a JS object keyed by the decimal number. Keys are D3D enum
// values, so they stay numbers rather than being decoded to names - the whole point of the
// measurement is to find out which ones occur, and a name table would have to be invented
// before knowing that.
template <typename Map, typename Fn>
JSValue MapToObject(JSContext *ctx, const Map &map, Fn value_of) {
  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  for (const auto &[key, value] : map) {
    JSValue v = value_of(ctx, value);
    if (JS_IsException(v) ||
        JS_SetPropertyUint32(ctx, obj, key, v) < 0) {
      JS_FreeValue(ctx, obj);
      return JS_EXCEPTION;
    }
  }
  return obj;
}

JSValue CountValue(JSContext *ctx, uint64_t n) {
  return JS_NewInt64(ctx, static_cast<int64_t>(n));
}

JSValue DistinctValue(JSContext *ctx, const std::set<uint32_t> &values) {
  return JS_NewInt32(ctx, static_cast<int32_t>(values.size()));
}

JSValue GetCaptured(JSContext *ctx, JSValueConst) {
  return JS_NewBool(ctx, d3d8::DeviceCreated());
}

JSValue GetReport(JSContext *ctx, JSValueConst) {
  const std::string text = d3d8::FormatStats();
  return JS_NewStringLen(ctx, text.data(), text.size());
}

// Structured form, so a REPL session can do arithmetic on it rather than reading prose -
// e.g. `Object.keys(render.stats.fvfs).length` to size the vertex-format work.
JSValue GetStats(JSContext *ctx, JSValueConst) {
  const d3d8::CaptureStats &s = d3d8::Stats();

  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }

  // Sets the property or unwinds the whole object; a partially-built stats object would be
  // worse than an exception, because it reads as a real measurement.
  bool failed = false;
  auto put = [&](const char *name, JSValue v) {
    if (failed) {
      JS_FreeValue(ctx, v);
      return;
    }
    if (JS_IsException(v) || JS_SetPropertyStr(ctx, obj, name, v) < 0) {
      failed = true;
    }
  };

  put("frames", JS_NewInt64(ctx, static_cast<int64_t>(s.frames)));
  put("draws_buffered", JS_NewInt64(ctx, static_cast<int64_t>(s.draws_buffered)));
  put("draws_user_ptr", JS_NewInt64(ctx, static_cast<int64_t>(s.draws_user_ptr)));
  put("max_draws_per_frame",
      JS_NewInt64(ctx, static_cast<int64_t>(s.max_draws_per_frame)));
  put("textures", JS_NewInt64(ctx, static_cast<int64_t>(s.textures_created)));
  put("vertex_buffers",
      JS_NewInt64(ctx, static_cast<int64_t>(s.vertex_buffers_created)));
  put("index_buffers", JS_NewInt64(ctx, static_cast<int64_t>(s.index_buffers_created)));
  put("vertex_bytes", JS_NewInt64(ctx, static_cast<int64_t>(s.vertex_bytes)));
  put("index_bytes", JS_NewInt64(ctx, static_cast<int64_t>(s.index_bytes)));
  put("max_texture_stage", JS_NewInt32(ctx, static_cast<int32_t>(s.max_stage_used)));
  put("max_light_index", JS_NewInt32(ctx, static_cast<int32_t>(s.max_light_index)));
  put("blocks_recorded", JS_NewInt64(ctx, static_cast<int64_t>(s.blocks_recorded)));
  put("blocks_opaque", JS_NewInt64(ctx, static_cast<int64_t>(s.blocks_opaque)));
  put("block_applies", JS_NewInt64(ctx, static_cast<int64_t>(s.block_applies)));
  put("block_states_total",
      JS_NewInt64(ctx, static_cast<int64_t>(s.block_states_total)));
  put("max_block_states", JS_NewInt64(ctx, static_cast<int64_t>(s.max_block_states)));
  put("distinct_materials", JS_NewInt64(ctx, static_cast<int64_t>(s.distinct_materials)));
  put("distinct_pipelines", JS_NewInt64(ctx, static_cast<int64_t>(s.distinct_pipelines)));
  put("max_materials_per_frame",
      JS_NewInt64(ctx, static_cast<int64_t>(s.max_materials_per_frame)));
  put("max_active_stages", JS_NewInt64(ctx, static_cast<int64_t>(s.max_active_stages)));
  put("opaque_block_applies",
      JS_NewInt64(ctx, static_cast<int64_t>(s.opaque_block_applies)));
  put("live_vertex_buffers",
      JS_NewInt64(ctx, static_cast<int64_t>(s.live_vertex_buffers)));
  put("live_index_buffers", JS_NewInt64(ctx, static_cast<int64_t>(s.live_index_buffers)));
  put("live_vertex_bytes", JS_NewInt64(ctx, static_cast<int64_t>(s.live_vertex_bytes)));
  put("live_index_bytes", JS_NewInt64(ctx, static_cast<int64_t>(s.live_index_bytes)));
  put("peak_live_vertex_bytes",
      JS_NewInt64(ctx, static_cast<int64_t>(s.peak_live_vertex_bytes)));
  put("peak_live_index_bytes",
      JS_NewInt64(ctx, static_cast<int64_t>(s.peak_live_index_bytes)));
  put("peak_live_buffers", JS_NewInt64(ctx, static_cast<int64_t>(s.peak_live_buffers)));
  put("locks", JS_NewInt64(ctx, static_cast<int64_t>(s.locks)));
  put("max_locked_bytes_per_frame",
      JS_NewInt64(ctx, static_cast<int64_t>(s.max_locked_bytes_per_frame)));
  put("foreign_buffers", JS_NewInt64(ctx, static_cast<int64_t>(s.foreign_buffers)));
  put("failed_uploads", JS_NewInt64(ctx, static_cast<int64_t>(s.failed_uploads)));
  put("unconvertible_buffers",
      JS_NewInt64(ctx, static_cast<int64_t>(s.unconvertible_buffers)));

  // The texture pixel path. The last three are the Phase 2c-iv question: each is a way
  // pixels reach a texture without IDirect3DTexture8::LockRect seeing them, so all three
  // being 0 is what says the bindless upload can be built on LockRect alone.
  put("live_textures", JS_NewInt64(ctx, static_cast<int64_t>(s.live_textures)));
  put("texture_lock_rects", JS_NewInt64(ctx, static_cast<int64_t>(s.texture_lock_rects)));
  put("texture_surface_levels",
      JS_NewInt64(ctx, static_cast<int64_t>(s.texture_surface_levels)));
  put("live_surfaces", JS_NewInt64(ctx, static_cast<int64_t>(s.live_surfaces)));
  put("surface_lock_rects", JS_NewInt64(ctx, static_cast<int64_t>(s.surface_lock_rects)));
  put("surface_texture_lock_rects",
      JS_NewInt64(ctx, static_cast<int64_t>(s.surface_texture_lock_rects)));
  put("surface_copy_rects", JS_NewInt64(ctx, static_cast<int64_t>(s.surface_copy_rects)));
  put("copy_rects_untracked",
      JS_NewInt64(ctx, static_cast<int64_t>(s.copy_rects_untracked)));
  put("copy_rects_partial", JS_NewInt64(ctx, static_cast<int64_t>(s.copy_rects_partial)));
  put("texture_render_targets",
      JS_NewInt64(ctx, static_cast<int64_t>(s.texture_render_targets)));
  put("texture_read_failures",
      JS_NewInt64(ctx, static_cast<int64_t>(s.texture_read_failures)));
  put("images_seeded", JS_NewInt64(ctx, static_cast<int64_t>(s.images_seeded)));
  put("textures_named", JS_NewInt64(ctx, static_cast<int64_t>(s.textures_named)));
  put("rim_records", JS_NewInt64(ctx, static_cast<int64_t>(s.rim_records)));
  put("rim_records_bound", JS_NewInt64(ctx, static_cast<int64_t>(s.rim_records_bound)));
  put("texture_updates", JS_NewInt64(ctx, static_cast<int64_t>(s.texture_updates)));
  put("resource_get_devices",
      JS_NewInt64(ctx, static_cast<int64_t>(s.resource_get_devices)));

  // fvfs and primitive_types map value -> how many draws used it; the state maps go to the
  // COUNT of distinct values seen, which is what says whether a state is a constant.
  put("fvfs", MapToObject(ctx, s.fvf_counts, CountValue));
  put("primitive_types", MapToObject(ctx, s.primitive_type_counts, CountValue));
  put("texture_formats", MapToObject(ctx, s.texture_formats, CountValue));
  put("texture_pools", MapToObject(ctx, s.texture_pools, CountValue));
  put("render_states", MapToObject(ctx, s.render_states, DistinctValue));
  put("stage_states", MapToObject(ctx, s.stage_states, DistinctValue));

  JSValue transforms = JS_NewArray(ctx);
  if (JS_IsException(transforms)) {
    failed = true;
  } else {
    uint32_t i = 0;
    for (const uint32_t state : s.transform_states) {
      if (JS_SetPropertyUint32(ctx, transforms, i++, JS_NewInt32(ctx, state)) < 0) {
        failed = true;
        break;
      }
    }
    put("transform_states", transforms);
  }

  if (failed) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
  return obj;
}

JSValue Reset(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  d3d8::ResetStats();
  return JS_UNDEFINED;
}

// Every texture image, with the `.rim` path it was acquired under. This is the surface a mod
// will eventually address the bindless table through - an asset name is the only identity a
// texture has that survives a restart and that someone can write down.
JSValue GetTextures(JSContext *ctx, JSValueConst) {
  const std::vector<vulkan::TextureImageInfo> images = vulkan::TextureImages();
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) {
    return array;
  }
  uint32_t i = 0;
  for (const vulkan::TextureImageInfo &info : images) {
    JSValue entry = JS_NewObject(ctx);
    if (JS_IsException(entry)) {
      JS_FreeValue(ctx, array);
      return entry;
    }
    JS_SetPropertyStr(ctx, entry, "index", JS_NewInt32(ctx, info.index));
    JS_SetPropertyStr(ctx, entry, "name",
                      JS_NewStringLen(ctx, info.name.c_str(), info.name.size()));
    JS_SetPropertyStr(ctx, entry, "width", JS_NewInt32(ctx, info.width));
    JS_SetPropertyStr(ctx, entry, "height", JS_NewInt32(ctx, info.height));
    JS_SetPropertyStr(ctx, entry, "levels", JS_NewInt32(ctx, info.levels));
    JS_SetPropertyStr(ctx, entry, "format", JS_NewInt64(ctx, info.d3d_format));
    JS_SetPropertyStr(ctx, entry, "bytes",
                      JS_NewInt64(ctx, static_cast<int64_t>(info.bytes)));
    if (JS_SetPropertyUint32(ctx, array, i++, entry) < 0) {
      JS_FreeValue(ctx, array);
      return JS_EXCEPTION;
    }
  }
  return array;
}

// The only check that the texture images hold the RIGHT bytes rather than merely holding
// bytes. Stalls the GPU once per mip level, so it is deliberately a call rather than a getter.
JSValue GetDrawReport(JSContext *ctx, JSValueConst) {
  const std::string report = vulkan::FormatDrawStats();
  return JS_NewStringLen(ctx, report.c_str(), report.size());
}

// Arms a RenderDoc capture of the next frame. Needs GKPLUS_RENDERDOC set at launch, because
// RenderDoc has to be loaded before the Vulkan instance exists.
JSValue Capture(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  if (!vulkan::TriggerCapture()) {
    return JS_ThrowTypeError(
        ctx, "renderdoc is not loaded - relaunch with GKPLUS_RENDERDOC=1");
  }
  return JS_UNDEFINED;
}

JSValue GetCaptureStatus(JSContext *ctx, JSValueConst) {
  const std::string status = vulkan::FormatCaptureStatus();
  return JS_NewStringLen(ctx, status.c_str(), status.size());
}

JSValue VerifyTextures(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  const std::string report = d3d8::VerifyTextureImages();
  return JS_NewStringLen(ctx, report.c_str(), report.size());
}

// The layer's own words, not just a count. See the note on ValidationMessages in VkContext.h:
// DebugWrite is OutputDebugString, and a debugger makes Gunlok unusable, so the REPL is the
// only practical way to read these.
JSValue GetValidationMessages(JSContext *ctx, JSValueConst) {
  const std::vector<std::string> &messages = vulkan::ValidationMessages();
  JSValue array = JS_NewArray(ctx);
  if (JS_IsException(array)) {
    return array;
  }
  uint32_t i = 0;
  for (const std::string &message : messages) {
    JSValue text = JS_NewStringLen(ctx, message.data(), message.size());
    if (JS_IsException(text) || JS_SetPropertyUint32(ctx, array, i++, text) < 0) {
      JS_FreeValue(ctx, array);
      return JS_EXCEPTION;
    }
  }
  return array;
}

JSValue ClearValidation(JSContext *ctx, JSValueConst, int, JSValueConst *) {
  vulkan::ClearValidationMessages();
  return JS_UNDEFINED;
}

// --- the Vulkan device -------------------------------------------------------
//
// Reading any of these brings the device up if it is not up already. That is deliberate:
// initialization cannot happen at DLL load (volkInitialize does LoadLibrary, which deadlocks
// under the loader lock), so *something* has to trigger it from the main thread, and a REPL
// query is the earliest useful trigger during bring-up.

JSValue GetVulkanReport(JSContext *ctx, JSValueConst) {
  const std::string text = vulkan::FormatCaps() + vulkan::FormatStats() +
                           vulkan::FormatResourceStats();
  return JS_NewStringLen(ctx, text.data(), text.size());
}

JSValue GetVulkan(JSContext *ctx, JSValueConst) {
  vulkan::Initialize();

  JSValue obj = JS_NewObject(ctx);
  if (JS_IsException(obj)) {
    return obj;
  }
  bool failed = false;
  auto put = [&](const char *name, JSValue v) {
    if (failed) {
      JS_FreeValue(ctx, v);
      return;
    }
    if (JS_IsException(v) || JS_SetPropertyStr(ctx, obj, name, v) < 0) {
      failed = true;
    }
  };

  put("status", JS_NewString(ctx, vulkan::InitResultName(vulkan::Status())));
  put("available", JS_NewBool(ctx, vulkan::Available()));
  const std::string &error = vulkan::LastError();
  put("error", error.empty() ? JS_NULL
                             : JS_NewStringLen(ctx, error.data(), error.size()));

  if (vulkan::Available()) {
    const vulkan::DeviceCaps &c = vulkan::Caps();
    put("device", JS_NewString(ctx, c.device_name.c_str()));
    put("discrete", JS_NewBool(ctx, c.discrete));
    put("api_version", JS_NewInt64(ctx, c.api_version));
    put("max_bindless_textures",
        JS_NewInt64(ctx, static_cast<int64_t>(c.max_bindless_textures)));
    put("max_push_constants",
        JS_NewInt64(ctx, static_cast<int64_t>(c.max_push_constants)));
    put("device_local_mb",
        JS_NewInt64(ctx, static_cast<int64_t>(c.device_local_bytes >> 20)));
    put("host_visible_device_local_mb",
        JS_NewInt64(ctx, static_cast<int64_t>(c.host_visible_device_local_bytes >> 20)));
    put("descriptor_indexing", JS_NewBool(ctx, c.descriptor_indexing));
    put("runtime_descriptor_array", JS_NewBool(ctx, c.runtime_descriptor_array));
    put("partially_bound", JS_NewBool(ctx, c.partially_bound));
    put("variable_descriptor_count", JS_NewBool(ctx, c.variable_descriptor_count));
    put("update_after_bind", JS_NewBool(ctx, c.update_after_bind));
    put("non_uniform_indexing", JS_NewBool(ctx, c.non_uniform_indexing));
    put("buffer_device_address", JS_NewBool(ctx, c.buffer_device_address));
    put("dynamic_rendering", JS_NewBool(ctx, c.dynamic_rendering));
    put("synchronization2", JS_NewBool(ctx, c.synchronization2));
  }

  const vulkan::RendererStats &r = vulkan::Stats();
  put("renderer_requested", JS_NewBool(ctx, vulkan::RendererRequested()));
  put("renderer_ready", JS_NewBool(ctx, r.ready));
  put("width", JS_NewInt64(ctx, r.width));
  put("height", JS_NewInt64(ctx, r.height));
  put("swapchain_images", JS_NewInt64(ctx, r.image_count));
  put("present_mode", JS_NewInt64(ctx, r.present_mode));
  put("frames_presented", JS_NewInt64(ctx, static_cast<int64_t>(r.frames_presented)));
  put("swapchain_rebuilds", JS_NewInt64(ctx, static_cast<int64_t>(r.swapchain_rebuilds)));
  put("acquire_failures", JS_NewInt64(ctx, static_cast<int64_t>(r.acquire_failures)));

  const vulkan::ResourceStats &m = vulkan::Resources();
  put("arenas_ready", JS_NewBool(ctx, m.ready));
  put("vertex_arena_bytes", JS_NewInt64(ctx, static_cast<int64_t>(m.vertex_arena_bytes)));
  put("index_arena_bytes", JS_NewInt64(ctx, static_cast<int64_t>(m.index_arena_bytes)));
  put("staging_bytes", JS_NewInt64(ctx, static_cast<int64_t>(m.staging_bytes)));
  put("vertex_used", JS_NewInt64(ctx, static_cast<int64_t>(m.vertex_used)));
  put("index_used", JS_NewInt64(ctx, static_cast<int64_t>(m.index_used)));
  put("uploads", JS_NewInt64(ctx, static_cast<int64_t>(m.uploads)));
  put("uploaded_bytes", JS_NewInt64(ctx, static_cast<int64_t>(m.uploaded_bytes)));
  put("staging_wraps", JS_NewInt64(ctx, static_cast<int64_t>(m.staging_wraps)));
  put("staging_flushes", JS_NewInt64(ctx, static_cast<int64_t>(m.staging_flushes)));
  put("staging_stalls", JS_NewInt64(ctx, static_cast<int64_t>(m.staging_stalls)));
  put("dropped_uploads", JS_NewInt64(ctx, static_cast<int64_t>(m.dropped_uploads)));
  put("arena_exhausted", JS_NewInt64(ctx, static_cast<int64_t>(m.arena_exhausted)));
  put("images_live", JS_NewInt64(ctx, static_cast<int64_t>(m.images_live)));
  put("images_created", JS_NewInt64(ctx, static_cast<int64_t>(m.images_created)));
  put("image_bytes", JS_NewInt64(ctx, static_cast<int64_t>(m.image_bytes)));
  put("image_peak_bytes", JS_NewInt64(ctx, static_cast<int64_t>(m.image_peak_bytes)));
  put("image_uploads", JS_NewInt64(ctx, static_cast<int64_t>(m.image_uploads)));
  put("image_uploaded_bytes",
      JS_NewInt64(ctx, static_cast<int64_t>(m.image_uploaded_bytes)));
  put("unsupported_formats", JS_NewInt64(ctx, static_cast<int64_t>(m.unsupported_formats)));
  put("unaligned_rects", JS_NewInt64(ctx, static_cast<int64_t>(m.unaligned_rects)));
  put("image_uploads_dropped",
      JS_NewInt64(ctx, static_cast<int64_t>(m.image_uploads_dropped)));
  put("descriptor_capacity",
      JS_NewInt64(ctx, static_cast<int64_t>(m.descriptor_capacity)));
  put("descriptors_written",
      JS_NewInt64(ctx, static_cast<int64_t>(m.descriptors_written)));
  put("samplers_live", JS_NewInt64(ctx, static_cast<int64_t>(m.samplers_live)));
  put("descriptors_out_of_range",
      JS_NewInt64(ctx, static_cast<int64_t>(m.descriptors_out_of_range)));

  if (failed) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
  return obj;
}

const JSCFunctionListEntry RenderProps[] = {
    JS_CGETSET_DEF("captured", GetCaptured, nullptr),
    JS_CGETSET_DEF("report", GetReport, nullptr),
    JS_CGETSET_DEF("stats", GetStats, nullptr),
    JS_CGETSET_DEF("vulkan", GetVulkan, nullptr),
    JS_CGETSET_DEF("vulkan_report", GetVulkanReport, nullptr),
    JS_CGETSET_DEF("validation", GetValidationMessages, nullptr),
    JS_CGETSET_DEF("textures", GetTextures, nullptr),
    JS_CGETSET_DEF("draws", GetDrawReport, nullptr),
    JS_CFUNC_DEF("reset", 0, Reset),
    JS_CFUNC_DEF("clear_validation", 0, ClearValidation),
    JS_CFUNC_DEF("verify_textures", 0, VerifyTextures),
    JS_CGETSET_DEF("renderdoc", GetCaptureStatus, nullptr),
    JS_CFUNC_DEF("capture", 0, Capture),
};

} // namespace

JSValue NewRenderNamespace(JSContext *ctx) {
  return NewNamespace(ctx, RenderProps, static_cast<int>(std::size(RenderProps)));
}

} // namespace gk::js
