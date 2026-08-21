#include "Render.h"

#include "Core.h"

namespace gk {
namespace {
// The builtin material globals, in enum order. They are consecutive-ish dwords
// from 0x00803d58 but NOT contiguous - 0x00803d68, 0x00803d74 and the run from
// 0x00803d7c to 0x00803d80 hold something else - so this is a table rather than
// base + index.
constexpr unsigned BuiltinMaterialOffsets[] = {
    0x00803d58, // Opaque
    0x00803d5c, // OpaqueUnlit
    0x00803d60, // OpaquePoint
    0x00803d64, // Translucent
    0x00803d6c, // TranslucentNoSort
    0x00803d70, // TranslucentPoint
    0x00803d78, // AdditiveAlpha
    0x00803d84, // UITranslucent
};
static_assert(std::size(BuiltinMaterialOffsets) ==
              static_cast<size_t>(BuiltinMaterial::UITranslucent) + 1);
} // namespace

RenderQueue *GetRenderQueue() {
  RenderQueue *queue;
  GetObjectAtOffset(queue, 0x00803e98);
  return queue;
}

// __fastcall: the queue arrives in ECX (MOV ESI,ECX at 0x005a90c2) and the
// function ends in a bare RET with no stack arguments to clean.
void FlushRenderQueue(RenderQueue *queue) {
  if (!queue) {
    return;
  }
  FastCall<void, RenderQueue *> flush;
  GetObjectAtOffset(flush, 0x005a90c0);
  flush(queue);
}

AwMaterial *GetCurrentMaterial() {
  AwMaterial **current;
  GetObjectAtOffset(current, 0x00803d54);
  return *current;
}

// __fastcall: MOV EDI,ECX at 0x005a3a31, then CMP [CurrentMaterial],EDI - the
// material is the only argument and it is in ECX. Bare RET.
//
// This is a no-op when the material is already bound, which is the whole point
// of the bucket sort; calling it out of band is safe but pointless.
void ApplyMaterial(AwMaterial *material) {
  if (!material) {
    return;
  }
  FastCall<void, AwMaterial *> apply;
  GetObjectAtOffset(apply, 0x005a3a30);
  apply(material);
}

AwMaterial *GetBuiltinMaterial(BuiltinMaterial which) {
  const auto index = static_cast<size_t>(which);
  if (index >= std::size(BuiltinMaterialOffsets)) {
    return nullptr;
  }
  AwMaterial **slot;
  GetObjectAtOffset(slot, BuiltinMaterialOffsets[index]);
  return *slot;
}

LightSet *GetCurrentLightSet() {
  LightSet **current;
  GetObjectAtOffset(current, 0x007c18bc);
  return *current;
}

LightSet *GetSceneLightSet() {
  LightSet **scene;
  GetObjectAtOffset(scene, 0x007c18cc);
  return *scene;
}

// Vtable slot 3 of LightSet, reached here through its one concrete
// implementation @0x0057a8c0 rather than through the vtable, because every
// LightSet in the game shares that implementation and going through the object
// would require trusting a pointer the caller supplied.
//
// __fastcall: MOV EBX,ECX at 0x0057a8c1, bare RET, no stack arguments.
//
// Note what this does beyond binding lights: it also calls SetD3DMaterial and
// publishes `set` as CurrentLightSet, so calling it mid-frame changes what
// DrawItemList_Render will consider "already bound" and can suppress the next
// item's own light change.
void ApplyLightSet(LightSet *set) {
  if (!set) {
    return;
  }
  FastCall<void, LightSet *> apply;
  GetObjectAtOffset(apply, 0x0057a8c0);
  apply(set);
}

int GetMaxD3DLights() {
  int *max;
  GetObjectAtOffset(max, 0x006ab97c);
  return *max;
}

unsigned GetRenderStateFlags() {
  unsigned *flags;
  GetObjectAtOffset(flags, 0x007b9c74);
  return *flags;
}

// __thiscall with NINE stack arguments: RET 0x24 at the tail, and 0x24 == 9 * 4.
// The argument order is not the order the fields land in - it was read off the
// prologue at 0x0059d78f, where [EBP+0x08] goes to DrawItem+0x0c, [EBP+0x0c] to
// +0x18 (camera), [EBP+0x10] to +0x1c (light set) and [EBP+0x14] to +0x14
// (material). Getting this wrong would swap the camera and the light set, which
// both being pointers would not fault - it would just render with the wrong one.
//
// That mapping has been re-measured against the prologue and is exactly right.
// Two of the arguments carry a sharper hazard than "renders with the wrong one",
// though: `arg7` (DrawItem+0x20) and `hooks` (DrawItem+0x2c) are **refcounted**
// by RenderQueue_Add, which addrefs the item's resources before routing it. So a
// non-null garbage pointer in either does not fault here - it increments a word
// at some offset inside whatever it points at, and the damage surfaces later and
// elsewhere. Passing null is safe; passing a plausible-looking pointer is not.
void SubmitDrawItem(Renderable *renderable, unsigned anim_time,
                    CameraData *camera, LightSet *light_set,
                    AwMaterial *material, int lod_level, unsigned flags,
                    int arg7, DrawItemHooks *hooks,
                    List<AwTexture *> *textures) {
  if (!renderable || !light_set || !material) {
    return; // RenderQueue_Add dereferences the light set and the material
  }
  ThisCall<void, Renderable *, unsigned, CameraData *, LightSet *, AwMaterial *,
           int, unsigned, int, DrawItemHooks *, List<AwTexture *> *>
      submit;
  GetObjectAtOffset(submit, 0x0059d760);
  submit(renderable, anim_time, camera, light_set, material, lod_level, flags,
         arg7, hooks, textures);
}
} // namespace gk
