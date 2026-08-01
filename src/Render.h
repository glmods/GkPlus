#pragma once

#include "List.h"
#include "Math.h"

#include <cstddef>
#include <cstdint>

// The high-level renderer - AWAPI, a library that ships beside the game code
// rather than inside it (`c:\projects\classics\gunlok\code\awapi\`, from the two
// surviving __FILE__ strings; the game is `Code\Gl\`). Unlike the RIF chunk
// layer, AWAPI has no counterpart in the published AvP source, so every layout
// here is decompiled.
//
// Read `rendering_notes.md` before changing anything in this file. The two facts
// that shape it:
//
//   * The frame is "submit everything, then drain". ~100 call sites push a
//     DrawItem through RenderQueue_Submit; RenderQueue_Flush state-sorts by
//     material, then texture, and draws. Nothing walks a scene graph per frame.
//   * There is almost no polymorphism. Nearly every vtable in the render range
//     is a one-slot List_Member<T> destructor; the D3D8 COM interfaces do the
//     virtual work. Only three real interfaces exist, and they are all here:
//     AwFrame's two transform slots, LightSet's six, and the DrawItem hook pair.
namespace gk {
struct CameraData;

// The engine-wide refcounted root: {vptr, refcount}, one virtual slot, and its
// root vtable is 0x006522e8. AwNode_Ctor installs that vtable at +0x9c and then
// overwrites it with the derived one, which is how the AWAPI inheritance chain
// is visible at all.
//
// `Map.h` declares its own `RefCountedBase` for the same 8 bytes. They are NOT
// merged on purpose: Map's second base subobject sits two levels above this root
// (0x006522e8 <- 0x0065281c <- 0x00652828) and its model folds the middle base's
// extra slot in, so it declares two virtuals where the root has one. Both are 8
// bytes, which is all either static_assert pins.
struct AwRefCounted {
  virtual ~AwRefCounted() = 0; // slot 0: scalar deleting destructor

  int refcount; // +0x04
};
static_assert(sizeof(AwRefCounted) == 0x8);

// The AwFrame/AwNode `flags` word at +0x8c. Every cached matrix has a "built"
// bit, and every input has a "this one is the source" bit; a setter ORs in its
// own source bit and masks out every derived bit at once.
enum AwFrameFlags : unsigned {
  // Cache-valid bits, each set by the routine that fills the matrix it names.
  AwFrame_MatrixValid = 0x00000001,        // AwFrame::matrix
  AwFrame_InverseValid = 0x00000002,       // AwFrame::inverse
  AwFrame_RotationBuilt = 0x00000004,      // the 3x3 part of `matrix`
  AwFrame_TranslationBuilt = 0x00000008,   // AwFrame::translation
  AwNode_ScaleMatrixBuilt = 0x00000010,    // AwNode::scale_matrix
  AwNode_WorldValid = 0x00000020,          // AwNode::world
  AwNode_InvScaleMatrixBuilt = 0x00000040, // AwNode::inv_scale_matrix
  AwNode_WorldInverseValid = 0x00000080,   // AwNode::world_inverse
  // Which input the build reads from.
  AwFrame_PositionIsSource = 0x00000100,    // AwFrame_SetPosition
  AwFrame_TranslationIsSource = 0x00000200, // AwFrame_SetTranslation
  AwFrame_EulersAreSource = 0x00000400,     // AwFrame_SetEulerAngles
  AwFrame_QuaternionIsSource = 0x00000800,  // AwFrame_SetQuaternion
  // Identity scale: AwNode::EnsureMatrix copies `matrix` into `world` verbatim
  // instead of multiplying by the scale matrix.
  AwNode_NoScale = 0x80000000,
};

// The base of the whole AWAPI transform hierarchy: a lazily-cached matrix and
// its inverse, rebuilt on demand under the dirty bits at +0x8c.
//
// **Both slots return a pointer to the composed 3x4 matrix**, and that return
// value is load-bearing: `DrawItem_RenderGeometry` calls slot 0 on the
// Renderable and feeds the pointer straight into Matrix3x4ToD3DMATRIX. It is
// the only virtual dispatch on the whole draw path that is neither a destructor
// nor a COM call. Which matrix comes back depends on the class - AwFrame returns
// `matrix`, AwNode returns `world` - which is the entire point of the override.
struct AwFrame {
  // Slot 0 -> &matrix. Early-outs on AwFrame_MatrixValid. Rebuilds the rotation
  // from `eulers` unless AwFrame_QuaternionIsSource, and the translation from
  // `position` unless AwFrame_TranslationIsSource.
  virtual float *EnsureMatrix() = 0;
  // Slot 1 -> &inverse. Calls slot 0 first, then transposes the 3x3 and rotates
  // the negated translation through it - valid only because the rotation part is
  // orthonormal. Sets AwFrame_InverseValid.
  virtual float *EnsureInverseMatrix() = 0;

  // +0x04 the authored position. AwFrame_SetPosition writes it and raises
  // AwFrame_PositionIsSource; `translation` below is then derived from it.
  Vec3 position;         // +0x04
  // BAM, 4096 to a turn - the same units the sin/cos tables index and that
  // MakeRole.h converts into. EnsureMatrix applies them Z, then Y, then X.
  int roll;              // +0x10
  int pitch;             // +0x14
  int yaw;               // +0x18
  Vec4 quaternion;       // +0x1c the rotation source under AwFrame_QuaternionIsSource
  float matrix[12];      // +0x2c 3x4, row-major - what slot 0 returns
  float inverse[12];     // +0x5c - what slot 1 returns
  unsigned flags;        // +0x8c see AwFrameFlags
  // +0x90 the translation the matrix actually uses: derived from `position`, or
  // written directly by AwFrame_SetTranslation under AwFrame_TranslationIsSource.
  // RenderQueue_Submit reads its `.y` sign-flipped as the depth sort key.
  Vec3 translation;      // +0x90
};
static_assert(sizeof(AwFrame) == 0x9c);
static_assert(offsetof(AwFrame, position) == 0x04);
static_assert(offsetof(AwFrame, roll) == 0x10);
static_assert(offsetof(AwFrame, quaternion) == 0x1c);
static_assert(offsetof(AwFrame, matrix) == 0x2c);
static_assert(offsetof(AwFrame, inverse) == 0x5c);
static_assert(offsetof(AwFrame, flags) == 0x8c);
static_assert(offsetof(AwFrame, translation) == 0x90);

// AwFrame plus a parent and a refcount. Overrides both transform slots to
// compose with the parent; the base versions still do the local part.
//
// The AwRefCounted subobject lands at +0x9c purely because AwFrame is 0x9c
// bytes, which puts `refcount` at +0xa0 - exactly the word RenderQueue_Add
// increments through DrawItem::renderable. The secondary vtable (0x0066da24) is
// one slot: an adjustor thunk to the deleting destructor.
// The four matrices are two derived pairs, not four independent transforms:
//
//   EnsureMatrix        -> scale_matrix = diag(scale)          [AwNode_ScaleMatrixBuilt]
//                          world = matrix * scale_matrix       [AwNode_WorldValid]
//   EnsureInverseMatrix -> inv_scale_matrix = diag(inv_scale)  [AwNode_InvScaleMatrixBuilt]
//                          world_inverse = inverse * inv_scale_matrix
//
// with AwNode_NoScale short-circuiting the multiply and copying `matrix`
// straight into `world`.
struct AwNode : AwFrame, AwRefCounted {
  Vec3 scale;             // +0xa4
  // +0xb0 componentwise reciprocal of `scale`, computed with the fast-reciprocal
  // table at 0x007fff80 rather than a divide. AwNode_SetScale writes both.
  Vec3 inv_scale;         // +0xb0
  float scale_matrix[12];      // +0xbc  diag(scale)
  float inv_scale_matrix[12];  // +0xec  diag(inv_scale)
  float world[12];             // +0x11c what EnsureMatrix returns
  float world_inverse[12];     // +0x14c what EnsureInverseMatrix returns
};
static_assert(sizeof(AwNode) == 0x17c);
static_assert(offsetof(AwNode, refcount) == 0xa0);
static_assert(offsetof(AwNode, scale) == 0xa4);
static_assert(offsetof(AwNode, inv_scale) == 0xb0);
static_assert(offsetof(AwNode, scale_matrix) == 0xbc);
static_assert(offsetof(AwNode, world) == 0x11c);

struct AwTexture;
struct AwMaterial;
struct SceneMesh;
struct SceneNode;
struct SubMesh;

// The submitted-to-the-queue object, and by a wide margin the most constructed
// thing in the renderer: 177 sites call Renderable_CtorFromShape @0x0059c0f0
// alone. `Map::scene_object` (+0xc8) and `Map::sky_object` (+0x188) are both
// one of these.
//
// It is RenderQueue_Submit's `this`: the sort key comes from AwFrame::translation
// .y sign-flipped, the refcount from the AwNode base, and the default texture
// list from `textures` below.
struct Renderable : AwNode {
  // +0x17c the root of the scene graph this renderable draws.
  // DrawItem_RenderGeometry hands it to SceneNode_Render as `this`, with the
  // D3DMATRIX it just built from EnsureMatrix as the parent transform. Owned:
  // the destructor releases it through SceneNode's vtable slot 2.
  SceneNode *root;         // +0x17c
  // +0x180 a malloc'd debug label, and the only thing the two "from" constructors
  // differ in: 21 bytes holding "Made from an AwShape" or "Made from an AwObject".
  // The destructor free()s it.
  char *debug_name;        // +0x180
  int field0x184;
  // +0x188 the tick the animation started on. Renderable_GetInterpolatedPosition
  // interpolates over (now - this).
  int anim_start_time;     // +0x188
  // +0x18c the textures this renderable can be bucketed under. When a producer
  // passes no list of its own, RenderQueue_Submit defaults to this one and
  // MaterialBucket_AddItem creates one TextureBucket per entry. The constructor
  // fills it from the root node's own list (SceneNode::textures) through
  // CollectTexturesFromNode.
  List<AwTexture *> textures; // +0x18c
  // +0x19c and +0x1a0 are counted Vec3 arrays: each is allocated with its element
  // count in the dword BEFORE the pointer, which is how the destructor frees them
  // (count * 0xc + 4). Renderable_GetVertex indexes the second.
  Vec3 *field0x19c;        // +0x19c
  Vec3 *vertices;          // +0x1a0
  // +0x1a4 raised at the end of the constructor, once `bounds_min`/`bounds_max`
  // and `box_corners` are filled. DrawItem_RenderGeometry tests it before using
  // them, so it reads as "the bounding volume is valid".
  bool has_bounds;         // +0x1a4
  uint8_t pad0x1a5[3];
  Vec3 bounds_min;         // +0x1a8
  Vec3 bounds_max;         // +0x1b4
  // +0x1c0 the eight corners of that box, expanded by MakeBoxCorners from the two
  // Vec3s above. 0x78 bytes; the destructor frees it with BoxCorners_Dtor.
  void *box_corners;       // +0x1c0
  bool field0x1c4;
  uint8_t pad0x1c5[3];
  // +0x1c8 and +0x1d4 are two more Vec3s - constructed by the constructor and
  // destructed by the destructor, and touched by nothing else at all. A second
  // bounds pair is the obvious guess and there is no evidence for it.
  Vec3 field0x1c8;
  Vec3 field0x1d4;
  // +0x1e0 the running animation controllers, one 0x3c-byte record per entry.
  // Renderable_AdvanceAnimations walks this list at the very top of
  // DrawItem_RenderGeometry, before the transform is bound, calling
  // Sequence_Advance on each with DrawItem::anim_time - which is why every
  // producer fills that argument from a scaled clock reading.
  List<void *> animations; // +0x1e0
};
static_assert(sizeof(Renderable) == 0x1f0);
static_assert(offsetof(Renderable, root) == 0x17c);
static_assert(offsetof(Renderable, textures) == 0x18c);
static_assert(offsetof(Renderable, bounds_min) == 0x1a8);
static_assert(offsetof(Renderable, box_corners) == 0x1c0);
static_assert(offsetof(Renderable, animations) == 0x1e0);

// What SceneNode::bounds points at, and what the view test consumes.
struct BoundingSphere {
  Vec3 centre;  // +0x00
  float radius; // +0x0c
};
static_assert(sizeof(BoundingSphere) == 0x10);

// The recursive half: a transform node owning up to ten level-of-detail meshes
// and a list of children. SceneNode_Render concatenates the parent matrix into
// `world`, picks lods[level], hands it to SceneMesh_Render and recurses.
//
// Unlike Renderable this derives from AwFrame directly - no refcount - and it
// adds a virtual destructor as slot 2, after the two inherited transform slots.
struct SceneNode : AwFrame {
  // +0x9c the node's own textures. A Renderable harvests these into its own list
  // at construction time (CollectTexturesFromNode), which is what makes the
  // renderable bucketable by texture without walking the graph every frame.
  List<AwTexture *> textures; // +0x9c
  // +0xac indexed by the LOD level SceneNode_Render is handed, which it passes
  // down unchanged to its children. Ten slots is the gap between here and
  // `world`, not a bound the code checks.
  SceneMesh *lods[10];    // +0xac
  float world[16];        // +0xd4 D3DMATRIX, set with SetTransform(D3DTS_WORLD)
  // +0x114 the frame counter (0x006aaa9c) this node's world matrix was last
  // composed on - the cache that stops a shared node being recomposed twice.
  int frame_stamp;        // +0x114
  // +0x118 the visibility verdict, non-zero meaning "skip". It is threaded into
  // every child as their incoming verdict, so a culled node prunes its whole
  // subtree without each child retesting.
  unsigned cull_result;   // +0x118
  // +0x120 the bounding sphere the view test uses: a {Vec3 centre, float radius}
  // record, and null disables culling for this node. SceneNode_Render reads the
  // radius from its +0x0c directly when the item carries no scale, and otherwise
  // rescales it per axis first.
  // +0x11c the node's name, and the key SceneNode_FindByName matches
  // case-insensitively before recursing into `children`. It is the same string a
  // RIF OBJHIERD node binding carries, which is how a hierarchy binds at runtime.
  char *name;             // +0x11c
  BoundingSphere *bounds; // +0x120
  // +0x124 and +0x130 are written together by FUN_0059b7d0, the routine that
  // matches on `name` and rotates the keyframe pair - so they are animated state
  // rather than authored, but which quantity is not established.
  Vec3 field0x124;        // +0x124 constructed as three floats
  int field0x130;
  // +0x134 OR'd into the flags word SceneNode_Render threads down. 0x4 selects
  // LOD 0 regardless, 0x10000 forces the D3D transform push, 0x1000003 skips
  // the culler, 0x20 bypasses the frame_stamp cache.
  // +0x134 initialised to 4, and bit 4 is the one SceneNode_Render tests to force
  // LOD 0 regardless of what the draw item asked for; the constructor clears it
  // again once the node has more than one LOD.
  unsigned flags;         // +0x134
  // +0x138..+0x178: the constructor takes the address of +0x138 and builds a Vec3
  // at +0x140 and a Vec4 at +0x14c, and the destructor tears those two down
  // again. A Vec3 + Vec4 pair is the shape of a position and a quaternion, but
  // NOTHING reads them - not the render walk, not the culler, not the frame
  // rotation - so the reading is unsupported and they stay unnamed.
  uint8_t unk0x138[0x8];
  Vec3 field0x140;        // +0x140
  Vec4 field0x14c;        // +0x14c
  uint8_t unk0x15c[0x1c];
  // +0x178 the per-node animation controllers, the list FUN_0059b5e0 prunes by
  // owner before recursing into `children`. Same 0x14-byte node payload the
  // Renderable's own animation list holds.
  List<void *> animations;      // +0x178
  List<SceneNode *> children;   // +0x188
  // +0x198 / +0x19c the node's current and previous evaluated keyframe, one
  // 0x14-byte record each. SceneNode_AdvanceFrame rotates them: the old current
  // becomes previous (or is freed), and a fresh current is built from it. Holding
  // both is what lets a frame be interpolated rather than snapped to.
  void *current_frame;    // +0x198
  void *previous_frame;   // +0x19c
};
static_assert(sizeof(SceneNode) == 0x1a0);
static_assert(offsetof(SceneNode, lods) == 0xac);
static_assert(offsetof(SceneNode, world) == 0xd4);
static_assert(offsetof(SceneNode, cull_result) == 0x118);
static_assert(offsetof(SceneNode, bounds) == 0x120);
static_assert(offsetof(SceneNode, flags) == 0x134);
static_assert(offsetof(SceneNode, children) == 0x188);

// One drawable mesh: a list of submeshes plus the bounds the lighting uses.
// SceneMesh_Render binds the world matrix, optionally asks the light set to pick
// lights for `bounds_a`/`bounds_b`, then walks `submeshes`.
struct SceneMesh {
  virtual ~SceneMesh() = 0; // slot 0, the only slot (vtable 0x0066da30)

  Vec4 field0x04;          // +0x04 constructed as four floats, never read here
  // +0x14 and +0x24 are two more lists the constructor builds exactly the way it
  // builds `submeshes`, and their node vtable (0x0065223c) is the generic
  // List_Member_Base one, so it says nothing about the payload. Nothing in the
  // draw path inserts into or walks them - they are filled by the geometry
  // builder (BuildShapeVertexBuffers), which is not dissected here.
  List<void *> field0x14;  // +0x14
  List<void *> field0x24;  // +0x24
  List<SubMesh *> submeshes; // +0x34
  // +0x44 .. +0x94, everything below, is build state:
  // SceneMesh_ResetBuildState clears the whole run and raises `needs_rebuild`.
  int field0x44;
  // +0x48 / +0x54 the pair SceneMesh_Render hands to the light set's
  // SelectLightsForBounds slot, together with the world matrix.
  Vec3 bounds_a;           // +0x48
  Vec3 bounds_b;           // +0x54
  int field0x60;
  int field0x64;
  Vec3 field0x68;          // +0x68 constructed as three floats
  uint8_t unk0x74[0x4];    // never written by any function read here
  // +0x78..+0x88 the ProcessVertices projection tail, run only when the caller
  // sets flag 0x20000000: the source vertex buffer, an index count, the index
  // array and a write cursor.
  void *proj_vertex_buffer; // +0x78
  int field0x7c;            // +0x7c cleared with the rest of the group
  int proj_index_count;     // +0x80
  uint16_t *proj_indices;   // +0x84
  int proj_cursor;          // +0x88
  int field0x8c;
  // +0x90 raised to 1 by SceneMesh_ResetBuildState, i.e. set whenever the build
  // state above has been thrown away.
  int needs_rebuild;        // +0x90
  int field0x94;
};
static_assert(sizeof(SceneMesh) == 0x98);
static_assert(offsetof(SceneMesh, submeshes) == 0x34);
static_assert(offsetof(SceneMesh, bounds_a) == 0x48);
static_assert(offsetof(SceneMesh, proj_vertex_buffer) == 0x78);

// A run of triangles sharing one texture. No vtable - SubMesh_Ctor writes a zero
// to +0x00, not a vptr.
//
// It is a TWO-MODE record and `vertex_buffer_owner` is the discriminator:
//
//   null  -> the user-pointer path. Aw_DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST,
//            fvf, vertices, vertex_count, indices, index_count).
//   set   -> the buffered path. Aw_DrawIndexedPrimitive(D3DPT_TRIANGLELIST,
//            <the VertexBufferSet two levels down>, index_buffer, base_vertex,
//            vertex_count, index_count, 0), and it early-outs when
//            `index_buffer` is null.
//
// `fvf` and `vertices` are dead in the buffered mode, where the FVF comes off the
// vertex buffer set instead. The primitive type is a constant 4 at both call
// sites, never a field.
struct SubMesh {
  void *vertices;      // +0x00 user-pointer mode only
  unsigned fvf;        // +0x04 user-pointer mode only
  // +0x08 reached as *(*(this+0x08) + 0x08) to get the VertexBufferSet whose own
  // +0x08 is the FVF - two levels of indirection, measured at 0x005a111e. The
  // intermediate object is not identified; it is NOT the AwSharedVB at
  // 0x00803c70, whose +0x08 is a CRITICAL_SECTION.
  void *vertex_buffer_owner; // +0x08
  int base_vertex;     // +0x0c passed to SetIndices as the base index
  int vertex_count;    // +0x10 NumVertices for the draw
  uint16_t *indices;   // +0x14 user-pointer mode; also what the wireframe path
                       //       reads to build its line list
  void *index_buffer;  // +0x18 buffered mode (an IndexBufferSet)
  // +0x1c the INDEX count, not a primitive count: SubMesh_DrawWireframe divides
  // it by 3 to get triangles, and Aw_Draw* runs it through
  // PrimitiveCountFromVertexCount. SceneMesh_Render accumulates it into the
  // frame counter at 0x00803c08, so that counter counts indices.
  int index_count;     // +0x1c
  // +0x20 what MaterialBucket_AddItem buckets on and what SceneMesh_Render
  // filters against when a TextureBucket replays.
  AwTexture *texture;  // +0x20
};
static_assert(sizeof(SubMesh) == 0x24);
static_assert(offsetof(SubMesh, vertex_buffer_owner) == 0x08);
static_assert(offsetof(SubMesh, index_count) == 0x1c);
static_assert(offsetof(SubMesh, texture) == 0x20);

// One D3D8 texture stage's worth of state. AwMaterial_ApplyStage pushes the
// whole record with SetTexture + SetTextureStageState, and the field order is
// the order it issues them in.
struct AwTextureStage {
  AwTexture *texture; // +0x00 null issues SetTexture(stage, nullptr)
  unsigned color_op;   // +0x04 D3DTSS_COLOROP
  unsigned color_arg1; // +0x08
  unsigned color_arg2; // +0x0c
  unsigned alpha_op;   // +0x10 D3DTSS_ALPHAOP
  unsigned alpha_arg1; // +0x14
  unsigned alpha_arg2; // +0x18
  // +0x1c..+0x24 clamped to 2 (D3DTEXF_LINEAR) when AnisotropicFilteringOn is
  // clear, which is how the video option reaches every material at once.
  unsigned min_filter; // +0x1c
  unsigned mag_filter; // +0x20
  unsigned mip_filter; // +0x24
  // +0x28 is not touched by AwMaterial_ApplyStage at all; +0x2c is read by it but
  // not issued as a stage state. The 0x30 stride is fixed by AwMaterial_Compile's
  // walk, so these two exist whatever they hold.
  int field0x28;
  int field0x2c;
};
static_assert(sizeof(AwTextureStage) == 0x30);

// AwMaterial_Ctor zeroes all 0x1bc bytes in one go, so the constructor says
// nothing about the layout - but the stage array pins the tail exactly:
// AwMaterial_Compile walks it from `this + 0x3c + num_stages * 0x30` downwards in
// 0x30 steps, and 0x3c + 8 * 0x30 is 0x1bc, the whole object.
// +0x0c..+0x28 are the nine D3D render states AwMaterial_Compile records into
// the state block, in the order it issues them. D3DRS_ZFUNC is not among them -
// it is hard-coded to D3DCMP_LESSEQUAL.
struct AwMaterial : AwRefCounted {
  // +0x08 how many of `stages` are live. AwMaterial_Compile iterates them
  // BACKWARDS, from the last down to stage 0.
  int num_stages;          // +0x08
  int z_write_enable;      // +0x0c D3DRS_ZWRITEENABLE
  int z_enable;            // +0x10 D3DRS_ZENABLE
  // +0x14 also picks the queue in RenderQueue_Add: 0 -> the opaque list, non-0
  // -> the blended list.
  int alpha_blend_enable;  // +0x14 D3DRS_ALPHABLENDENABLE
  int alpha_test_enable;   // +0x18 D3DRS_ALPHATESTENABLE
  int src_blend;           // +0x1c D3DRS_SRCBLEND
  int dest_blend;          // +0x20 D3DRS_DESTBLEND
  int clipping;            // +0x24 D3DRS_CLIPPING
  int lighting;            // +0x28 D3DRS_LIGHTING
  // +0x2c overrides alpha_blend_enable: the item goes to the depth-sorted list.
  int needs_depth_sort;    // +0x2c
  // +0x30 the compiled D3D8 state block. AwMaterial_Compile deletes the old one
  // and records a new one; the destructor passes it to DeleteStateBlock.
  unsigned state_block;    // +0x30
  // +0x34 the translucent counterpart. MaterialBucket_AddItem diverts an item to
  // the sorted list when a texture is translucent and this is set, and
  // DrawItemList_Render substitutes it for an opaque material that got there.
  AwMaterial *blended_variant; // +0x34
  // +0x38 makes RenderQueue_Add CLONE the item with this material and re-add it,
  // which is how a multi-pass material fans out into extra draws.
  // AwMaterial_Compile also recurses into it, so compiling one material compiles
  // the whole chain.
  AwMaterial *next_pass;       // +0x38
  AwTextureStage stages[8];    // +0x3c the D3D8 stage limit, exactly filling it
};
static_assert(sizeof(AwMaterial) == 0x1bc);
static_assert(offsetof(AwMaterial, num_stages) == 0x08);
static_assert(offsetof(AwMaterial, stages) == 0x3c);
static_assert(offsetof(AwMaterial, z_write_enable) == 0x0c);
static_assert(offsetof(AwMaterial, alpha_blend_enable) == 0x14);
static_assert(offsetof(AwMaterial, lighting) == 0x28);
static_assert(offsetof(AwMaterial, needs_depth_sort) == 0x2c);
static_assert(offsetof(AwMaterial, state_block) == 0x30);
static_assert(offsetof(AwMaterial, blended_variant) == 0x34);
static_assert(offsetof(AwMaterial, next_pass) == 0x38);

// AwTexture IS the texture cache's record - the two are the same 0x34-byte
// object, not the separate types an earlier revision of this file assumed. The
// chain that proves it: AcquireRimTexture mints the record (path at +0x2c, flags
// at +0x28), AwShape_TouchTextures looks each of a shape's textures up in the
// same hash by that +0x2c path and bumps +0x1c, BuildShapeVertexBuffers stores
// them into SubMesh::texture, and AwMaterial_ApplyStage finally takes the D3D
// texture as `**stage` - straight off offset 0.
//
// That last step is also why it is NOT one of the AwRefCounted family: offset 0
// holds the D3D interface, so there is no vptr and no refcount at +0x04.
struct AwTexture {
  void *d3d_texture; // +0x00 the IDirect3DBaseTexture8 SetTexture is given
  uint8_t unk0x04[0x14];
  // +0x18 non-zero routes the whole draw item to the depth-sorted list when the
  // material has a blended_variant, and is the filter for the translucent pass.
  int translucent; // +0x18
  // +0x1c the cache's reference count: AcquireRimTexture bumps it on a hit, and
  // AwShape_TouchTextures bumps it when a shape's texture is found and RESETS it
  // to 0 when it is not - so it counts live users of a cached entry, and a miss
  // deliberately starts the count over.
  int refcount;    // +0x1c
  // +0x20 also incremented per use, by BuildShapeVertexBuffers. Only that one
  // site is established, so it is kept apart from `refcount`.
  int field0x20;
  uint8_t unk0x24[0x4];
  unsigned flags;  // +0x28 the flags AcquireRimTexture was called with
  char *path;      // +0x2c strdup'd, and the key the cache hashes
  uint8_t unk0x30[0x4];
};
static_assert(sizeof(AwTexture) == 0x34);
static_assert(offsetof(AwTexture, translucent) == 0x18);
static_assert(offsetof(AwTexture, refcount) == 0x1c);
static_assert(offsetof(AwTexture, path) == 0x2c);

// The 0x6c-byte record LightSet_AddLight allocates. Only its size and its owner
// are established here.
struct Light;

// The one genuinely polymorphic interface in the renderer: six slots, and
// lighting is state-sorted through it exactly the way materials and textures are.
//
// It is also, structurally, a D3D material with a light list bolted on: the
// whole 0x44-byte tail is a D3DMATERIAL8 and SetD3DMaterial hands `this + 0x18`
// straight to IDirect3DDevice8::SetMaterial. That is why LightSet_Apply binds a
// material as well as lights, and why "ambient light" in the console commands is
// really this object's Emissive term.
struct LightSet : AwRefCounted {
  // Slot 1. SceneMesh_Render calls this per mesh with the mesh's own bounds and
  // world matrix - but only while CurrentLightSet == SceneLightSet, i.e. only
  // when no draw item has overridden the lighting.
  virtual void SelectLightsForBounds(const Vec3 *bounds_a, const Vec3 *bounds_b,
                                     const float *world) = 0;
  // Slot 2. Unlinks `light` from the list and frees its 0x6c bytes.
  virtual void RemoveLight(Light *light) = 0;
  // Slot 3. Fills D3D light slots 0..n from `lights`, LightEnable(false) on the
  // rest up to MaxD3DLights, applies the D3D material, then publishes itself as
  // CurrentLightSet. DrawItemList_Render calls it whenever a DrawItem's own
  // light set differs from the current one.
  virtual void Apply() = 0;
  virtual void Disable() = 0;
  // Slot 5. Appends a new 0x6c-byte Light, or returns null once the list already
  // holds MaxD3DLights of them - the cap is enforced here, not at bind time.
  virtual Light *AddLight() = 0;

  List<Light *> lights; // +0x08

  // +0x18 a D3DMATERIAL8, laid out exactly as D3D declares it. Named rather than
  // typed so this header does not have to pull in d3d8.h.
  Vec4 diffuse;   // +0x18
  Vec4 ambient;   // +0x28
  Vec4 specular;  // +0x38
  Vec4 emissive;  // +0x48 what GetAmbientLight / SetAmbientLight read and write
  float power;    // +0x58 seeded to 1.0 by the constructor
};
static_assert(sizeof(LightSet) == 0x5c);
static_assert(offsetof(LightSet, lights) == 0x08);
static_assert(offsetof(LightSet, diffuse) == 0x18);
static_assert(offsetof(LightSet, emissive) == 0x48);
static_assert(offsetof(LightSet, power) == 0x58);

// The per-DrawItem callback pair the engine supplies to itself: slot 1 runs
// immediately before the geometry and slot 2 immediately after, both with the
// Renderable as their argument. 46 of the 102 submit sites pass one.
struct DrawItemHooks : AwRefCounted {
  virtual void PreDraw(Renderable *renderable) = 0;  // slot 1
  virtual void PostDraw(Renderable *renderable) = 0; // slot 2
};

// The queue's element. Pool-allocated per submit, refcounted, and freed by the
// flush - the queue is rebuilt from scratch every frame.
struct DrawItem : AwRefCounted {
  // +0x08 the submitter, and DrawItem_RenderGeometry's `this`. RenderQueue_Add
  // addrefs it through AwNode::refcount (+0xa0).
  Renderable *renderable;  // +0x08
  // +0x0c the time the renderable's animations are evaluated at.
  // DrawItem_RenderGeometry passes it to Renderable_AdvanceAnimations before it
  // touches the transform, and Sequence_Advance scales it into the same 16.16
  // domain a RIF OBASEQFR keyframe's `time` uses. Every producer fills it from
  // FUN_00571b60, a scaled per-thread clock reading - which is exactly why it
  // looks like an object pointer at the call sites and is not one.
  unsigned anim_time;      // +0x0c
  // +0x10 0x40000000 arms the DAT_006ac654 side channel, 0x4000000 is OR'd in by
  // DrawItemList_Render before the draw, the sign bit forces Camera_SaveTransforms.
  unsigned flags;          // +0x10
  AwMaterial *material;    // +0x14
  CameraData *camera;      // +0x18 switched only when it differs from the current
  LightSet *light_set;     // +0x1c addrefed; never null
  int field0x20;           // +0x20 addrefed when set; reaches DAT_006ac654
  // +0x24 which entry of SceneNode::lods to draw. DrawItem_RenderGeometry threads
  // it into SceneNode_Render, which passes it unchanged to every child - so one
  // item picks the LOD for the whole subtree. Flag 0x4 on the node forces 0.
  int lod_level;           // +0x24
  // +0x28 Renderable::translation.y XOR 0x80000000. Ascending key therefore means
  // descending distance, which is what makes SortedDrawList_Insert back-to-front.
  float sort_key;          // +0x28
  DrawItemHooks *hooks;    // +0x2c optional, addrefed
};
static_assert(sizeof(DrawItem) == 0x30);
static_assert(offsetof(DrawItem, renderable) == 0x08);
static_assert(offsetof(DrawItem, flags) == 0x10);
static_assert(offsetof(DrawItem, material) == 0x14);
static_assert(offsetof(DrawItem, light_set) == 0x1c);
static_assert(offsetof(DrawItem, sort_key) == 0x28);
static_assert(offsetof(DrawItem, hooks) == 0x2c);

// The second level of the state sort, keyed on texture: one bind per bucket.
struct TextureBucket {
  AwTexture *texture;      // +0x00
  List<DrawItem *> items;  // +0x04
};
static_assert(sizeof(TextureBucket) == 0x14);

// The first level, keyed on material. Found by linear search over the queue's
// list - the buckets are push-front and unsorted.
struct MaterialBucket {
  AwMaterial *material;         // +0x00
  List<TextureBucket *> buckets; // +0x04
};
static_assert(sizeof(MaterialBucket) == 0x14);

// The one global queue. Refilled from scratch every frame, and NOT drained only
// once: SubmitAndFlushMapGeometry calls the flush itself, so the world geometry
// goes out ahead of everything submitted after it.
struct RenderQueue {
  List<MaterialBucket *> opaque;  // +0x00 drained first
  List<MaterialBucket *> blended; // +0x10 then these
  // +0x20 drawn LAST, ordered on DrawItem::sort_key - the translucency pass.
  // Its address, 0x00803eb8, is the `RenderQueue_sorted` symbol.
  List<DrawItem *> sorted;        // +0x20
};
static_assert(sizeof(RenderQueue) == 0x30);
static_assert(offsetof(RenderQueue, sorted) == 0x20);

// The engine's own materials, in the order they sit in .data from 0x00803d58.
// `CurrentMaterial` @ 0x00803d54 is the one bound right now and is deliberately
// not a member of this enum.
enum class BuiltinMaterial {
  Opaque,          // 0x00803d58
  OpaqueUnlit,     // 0x00803d5c
  OpaquePoint,     // 0x00803d60
  Translucent,     // 0x00803d64
  TranslucentNoSort, // 0x00803d6c
  TranslucentPoint,  // 0x00803d70
  AdditiveAlpha,     // 0x00803d78
  UITranslucent,     // 0x00803d84
};

// ---------------------------------------------------------------------------
// Native API. Every wrapped function's calling convention was checked against
// its RET form per the rule in CLAUDE.md; the operands are recorded beside each
// one in Render.cpp.

RenderQueue *GetRenderQueue();
// Drains and frees the queue. Safe to call more than once a frame - the game
// does exactly that.
void FlushRenderQueue(RenderQueue *queue);

AwMaterial *GetCurrentMaterial();
void ApplyMaterial(AwMaterial *material);
AwMaterial *GetBuiltinMaterial(BuiltinMaterial which);

LightSet *GetCurrentLightSet();
LightSet *GetSceneLightSet();
void ApplyLightSet(LightSet *set);
int GetMaxD3DLights();

unsigned GetRenderStateFlags();

// The universal "draw this" verb - 102 call sites across 31 producers. All nine
// arguments are positional in the engine; this wrapper only names them.
// `textures` may be null, in which case the engine defaults to
// renderable->textures. `hooks` may be null.
void SubmitDrawItem(Renderable *renderable, unsigned anim_time,
                    CameraData *camera, LightSet *light_set,
                    AwMaterial *material, int lod_level, unsigned flags,
                    int arg7, DrawItemHooks *hooks,
                    List<AwTexture *> *textures);
} // namespace gk
