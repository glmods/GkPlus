#pragma once

#include "Field.h"
#include "HashTable.h"
#include "List.h"
#include "Math.h"
#include "Memory.h"
#include "Vulnerability.h"

#include <cstddef>

namespace gk {
struct Role;
struct Character;

// The 0x44-byte inventory container hanging off MobileActor::inventory. Only its
// size is known (from the pool free in MobileActor::Destructor @ 0x00532b00), so
// it stays opaque - the forward declaration exists to give the owning pointer a
// name to point at.
struct Inventory;

// 0x28-byte weapon instance hanging off CharacterActor::weapon_data.
struct Weapon {
  field field0x0;
  float range;        // 0x04 base range, scaled by CharacterActor::weapon_range
  int weapon_type;    // 0x08 cases 4..0x20; 0xe is special-cased
  float ammo_rate;    // 0x0c from AmmoInfos, scaled by the per-thread delta time
  float ammo_param;   // 0x10 second AmmoInfos float
  field field0x14;
  field field0x18;    // 0x18/0x1c thresholds compared against the ammo count
  field field0x1c;
  int ammo_type;      // 0x20 0x12 = none/melee
  field field0x24;
};
static_assert(sizeof(Weapon) == 0x28);

// Every slot below was checked against the `RET` operand of the function the
// game puts in it. __thiscall is callee-clean, so `RET n` is ground truth for
// how many bytes of arguments the callee pops, and a declaration that disagrees
// drifts ESP by the difference on **every call** - the failure described under
// "Function Calling Convention" in CLAUDE.md, which surfaces as an access
// violation with EIP on the stack, nowhere near the call.
//
// Nine declarations were wrong and are fixed below. Where the argument's purpose
// is not known it is `int` with a comment: the arity is measured, the type is
// not, and one dword is what the slot pops either way. Slot 0 is exempt - MSVC's
// scalar deleting destructor always takes a hidden `int flags`.
struct Actor {
  // 0x04 reference count. Retain is `INC [ESI+4]`; release is
  // `ADD [ECX+4],-1 ; JNZ ; CALL [vtbl+0]` with the scalar-delete flag, in
  // FUN_0045e050 @ 0x0045e174-89. Do not adjust it from GkPlus - the release
  // path runs the destructor.
  int ref_count;
  bool field0x8;
  char pad0x9[3];
  int id;                     // 0x0c
  // Actor::Ctor allocates the sentinel and copies the role's entries into it -
  // the `Vulnerability` objects are shared, not cloned. Nothing here is a
  // pool_unique_ptr: the sentinel is pool_alloc'd and then never freed, and the
  // entries are only pool-freed by ~Actor when `actor_scoped` is set (the
  // role-supplied ones stay owned by the Role).
  VulnerabilityList vulnerabilities; // 0x10
  int field0x20;
  // 0x24 a float deadline in seconds, 0.0 meaning "disabled". The AI timer
  // @ 0x00456bed does `MOVSS XMM0,[ESI+0x24]`, compares it against the scaled
  // clock, and fires vtable slot 79 when it has passed.
  float slot79_deadline;
  // 0x28/0x2c: written only by ChangeOwnerAndTeam (slot 80), which also puts
  // them in its 0x58 update. The DB's name for that slot says "owner"; nothing
  // measured here confirms it, so they keep the field0xNN convention. They are
  // exposed because a team change has to preserve them - see JsActors.cpp.
  //
  // **+0x2c has a second, conflicting reading and is deliberately left `int`.**
  // The same AI timer reads it at 0x00456c21 as a float deadline driving vtable
  // slot 81, exactly as it reads +0x24 above. Both observations are measured -
  // one is a writer, one is a reader - and nothing has reconciled them. Retyping
  // it to `float` would make JsActors.cpp's `ChangeOwnerAndTeam(field0x28,
  // field0x2c, team)` round-trip convert numerically instead of preserving the
  // bits, so the storage type stays as the writer needs it until the conflict is
  // settled.
  int field0x28;
  int field0x2c;
  float alarm_delay;          // 0x30
  int field0x34;              // 0x34 the AI think proc, per AIType
  // 0x38 an optional "on placed" hook, called as `hook(this, now_lo, now_hi)`
  // (matching its RET 0x8) from Actor::InitPositionAndTiming @ 0x0052dd18 right
  // after the nav-poly lookup, and from Actor_FixupAfterLoad @ 0x00531860 on a
  // savegame restore. It is a *placement-time* hook, not a per-tick one:
  // ExecutorActorTick reads only vtable displacements 0x18/0x90/0xdc/0x118 and
  // never touches +0x34 or +0x38. The only installer in the shipped build is
  // AIType::Mine, which puts Mine_OnDeployed @ 0x0045a640 here
  // (Actor_SetAiBehaviour @ 0x00450610).
  void(__thiscall *on_placed)(Actor *, unsigned now_lo, int now_hi);
  int field0x3c;              // 0x3c gates the team-list move - see JsActors.cpp
  void *attachment;           // 0x40 ref-counted; released by slot 52
  int field0x44;
  char pad0x48[8];
  int ai_type;                // 0x50
  int field0x54;
  char pad0x58[8];
  float aggression;           // 0x60
  Vec3 field0x64;
  char pad0x70[0xc];
  unsigned flags;             // 0x7c slot 82 tests 0x8, sets 0x200
  int field0x80;
  char pad0x84[4];
  float field0x88;            // 0x88 last-action timestamp (slot 93 gate)
  char pad0x8c[4];
  int field0x90;
  int field0x94;
  float field0x98;
  char pad0x9c[4];
  Vec3 position;              // 0xa0
  Vec4 orientation;           // 0xac
  int team_id;                // 0xbc
  Role *role;                 // 0xc0 (`entity` in the Ghidra DB)
  Role *frag_role;            // 0xc4
  int field0xc8;              // 0xc8/0xcc 64-bit last-update timestamp
  int field0xcc;
  int field0xd0;
  int field0xd4;
  // 0xd8 the game time in seconds, written **every tick** by the base slot-70
  // body Actor::Update @ 0x0052f91a (`MOV [EDI+0xd8],EAX` with EAX = the `now`
  // argument). It is also the first field of the 0x30-byte MotionSnapshot.
  float state_time;
  // 0xdc state_time as of the last state broadcast - broadcast dirty-tracking,
  // written at 0x0052fa66 and by all four motion broadcasters, each of which
  // ends `0xdc = 0xd8`. It was described here as "latched into 0xdc on death",
  // which is one of nine writers (Die) mistaken for the rule.
  float broadcast_time;
  // 0xe0 3D model, driven by slots 71-74. pool_alloc'd (0x1f0) by Actor::Ctor but
  // refcounted from then on - the dtor decrements and calls slot 0, so the free
  // is the model's business, not the Actor's.
  void *anim_object;
  Vec3 top_coords;            // 0xe4
  float armor_value;          // 0xf0
  float strength;             // 0xf4 current health pool
  float health;               // 0xf8
  int field0xfc;
  char pad0x100[0xc];
  bool position_set;          // 0x10c
  bool visible;               // 0x10d slot 61
  char pad0x10e[2];
  float field0x110;
  char field0x114;
  bool is_dead;               // 0x115 set by slot 65; slot 6 returns !is_dead
  char pad0x116[2];
  int field0x118;             // 0x118 slot 12
  char pad0x11c[4];

  // vtable, slots 0-82. Each comment is `slot#  what the *base* body does`; where
  // a subclass gives the slot a materially different meaning that is called out
  // inline. Slot indices are branch-local (see actor_vtable_notes.md for the
  // per-slot addresses and the network-message ids).

  // 0  scalar deleting dtor: run ~Actor's body, then optionally pool-free 0x120.
  virtual ~Actor() = 0;
  // 1  post-construction hook; base is a no-op.
  virtual void OnCreate() = 0;
  // 2  write health (+0xf8), broadcast 0x55.
  virtual void SetHealth(float) = 0;
  // 3  out-param health from +0xf8.
  virtual void GetHealth(float *) = 0;
  // 4  position with Y raised by half the actor height.
  virtual Vec3 *GetCenterCoords(Vec3 *) = 0;
  // 5  out-param current/max strength.
  virtual void GetStrengthRatio(float *) = 0;
  // 6  !is_dead (+0x115).
  virtual bool IsAlive() = 0;
  // 7  base false; CharacterActor returns is_attacking (+0x2d4).
  virtual bool IsAttacking() = 0;
  // 8  base false; MobileActor returns is_concealed (+0x186). Getter paired with slot 9.
  //    Was IsMine, which described nothing: an actual mine is an ai-mine role, and this
  //    is the camouflage flag the AI skips on. See stealth_and_fog_notes.md.
  virtual bool IsConcealed() = 0;
  // 9  base RET 4 (discards arg); MobileActor stores it to is_concealed. Setter for slot 8.
  virtual void SetConcealed(bool) = 0;
  // 10  role->character.
  virtual Character *GetCharacter() = 0;
  // 11  base NULL; CharacterActor returns its 0x28-byte weapon (+0x2b8).
  virtual Weapon *GetWeapon() = 0;
  // 12  +0x118 (the nav poly cached by slot 51).
  virtual int GetField0x118() = 0;
  // 13  base true; PickupActor returns its enabled flag (+0x120).
  virtual bool IsEnabled() = 0;
  // 14  base false; MobileActor returns is_moving (+0x184).
  virtual bool IsMoving() = 0;
  // 15  base NULL; CharacterActor returns attack_target (+0x2d8).
  virtual Actor *GetAttackTarget() = 0;
  // 16  base NULL; MobileActor returns the 0x44-byte inventory container (+0x194).
  virtual void *GetInventory() = 0;
  // 17  base false; MobileActor: character->customisation_hierarchy != NULL.
  virtual bool HasCustomisationHierarchy() = 0;
  // 18  out-param armor_value (+0xf0).
  virtual void GetArmorValue(float *) = 0;
  // 19  base out-params 0.0; CharacterActor returns shield_value (+0x2d0).
  virtual void GetShieldValue(float *) = 0;
  // 20  base no-op; CharacterActor destroys an armor piece, broadcast 0xa6/0xa8.
  virtual void ApplyArmorDamage(int) = 0; // RET 0x4: one argument, purpose unknown
  // 21  base no-op; CharacterActor destroys a shield piece, broadcast 0xa6/0xa7.
  virtual void ApplyShieldDamage(int) = 0; // RET 0x4: one argument, purpose unknown
  // 22  base returns 100; CharacterActor returns the real ammo count.
  virtual int GetAmmoCount() = 0;
  // 23  base 0; CharacterActor returns the cannot-fire gate (+0x304). Paired w/ slot 95.
  virtual int GetField0x304() = 0;
  // 24  base NULL; MobileActor returns its nav agent (+0x200).
  virtual void *GetNavAgent() = 0;
  // 25  base 0; MobileActor returns a team-slot index (+0x18c). Paired with slot 26.
  virtual int GetField0x18c() = 0;
  // 26  base no-op (ignores arg); MobileActor stores the team-slot index. Setter for 25.
  virtual void SetField0x18c(int) = 0;
  // 27  no-op, never overridden anywhere.
  virtual void Stub27(int) = 0; // RET 0x4: one argument, purpose unknown
  // 28  write armor_value (+0xf0). Setter for slot 18.
  virtual void SetArmorValue(float) = 0;
  // 29  base no-op; CharacterActor writes shield_value (+0x2d0). Setter for slot 19.
  virtual void SetShieldValue(float) = 0;
  // 30  base false; MobileActor returns +0x188. Getter paired with slot 54.
  virtual bool GetField0x188() = 0;
  // 31  base NULL; CharacterActor returns its hotspot node (+0x2c8).
  virtual char *GetHotspot() = 0;
  // 32  base false; MobileActor tests the order-queue count (+0x1f4).
  virtual bool HasPendingOrders() = 0;
  // 33  team_id = arg; MobileActor/CharacterActor/PresidentActor extend it.
  virtual void SetTeamID(int) = 0;
  // 34  base NULL; MobileActor returns &inventory_list (+0x19c).
  virtual void *GetInventoryListPtr() = 0;
  // 35  sizeof(this): 0x120 in base. Drives polymorphic (de)allocation without RTTI.
  virtual int GetSize() = 0;

  // 36-50  manual RTTI: each returns false in Actor and true only in the named
  // class (and its descendants, which inherit the override) - the binary has no
  // C++ RTTI, so these slots are the whole type-check mechanism.
  virtual bool IsMobile() = 0;
  virtual bool IsCharacter() = 0;
  virtual bool IsProjectile() = 0;
  virtual bool IsTrackObject() = 0;
  virtual bool IsNode() = 0;
  virtual bool IsCentipede() = 0;
  virtual bool IsCentibody() = 0;
  virtual bool IsBackgroundCreature() = 0;
  virtual bool IsFlyingBackgroundCreature() = 0;
  virtual bool IsPickup() = 0;
  virtual bool IsTumbleweed() = 0;
  virtual bool IsPopup() = 0;
  virtual bool IsBlocker() = 0;
  virtual bool IsPresident() = 0;
  virtual bool IsTurret() = 0;

  // 51  stamp the create/update timestamps, resolve the nav poly under the actor
  //     into +0x118, and fire the +0x38 callback if one is installed.
  virtual void InitPositionAndTiming(int, int, float) = 0;
  // 52  refcounted release of the attachment at +0x40.
  virtual void ReleaseAttachment() = 0;
  // 53  set coords + orientation quaternion + timing.
  virtual void SetPositionAndOrientation(Vec3 *, Vec4 *, int) = 0;
  // 54  base RET 4 (discards); MobileActor stores it to +0x188. Setter for slot 30.
  virtual void SetField0x188(bool) = 0;
  // 55  base no-op; ProjectileActor's is the physics step (integrate/collide/damage).
  virtual void OnPrePhysics(int, int, int) = 0; // RET 0xc: three arguments
  // 56  path to a target and start moving. The base @ 0x0054efa0 is a bare
  //     `RET 0x8` stub; MobileActor overrides at 0x00539930 with the real body -
  //     early-out on a null nav poly, shorten the goal by a fixed margin when
  //     stop_range_sq is large, allocate a 0xc-byte List_Member_Base sentinel,
  //     read the target's centre through its slot 4, then call
  //     FindNavPathWithinRadius @ 0x0052c100 with nav_agent->traversal_flags,
  //     SetMoveState(0), ClearRouteWaypoints, and push each returned node
  //     through slot 90 AddWaypoint. There is no collision manifold, no impulse
  //     and no contact normal anywhere in it; it was named OnCollisionResponse
  //     here until it was read.
  virtual void PathToTarget(Actor *target, float stop_range_sq) = 0; // RET 0x8
  // 57  ray/shape intersection; Pickup/Projectile return 0 to opt out of being hit.
  virtual void Raycast(int *, int, int, int *) = 0;
  // 58  swept intersection; same opt-out as slot 57.
  virtual void SweepTest(int *, int, int, int, int *) = 0;
  // 59  base no-op; in PickupActor this slot is really SetPickupType.
  virtual void OnDamageReceived(int, int) = 0; // RET 0x8: two arguments
  // 60  base false; MobileActor: alive && +0x17c != a global sentinel.
  virtual bool IsTargetable() = 0;
  // 61  visible flag (+0x10d).
  virtual bool IsVisible() = 0;
  // 62  base false; MobileActor: move_state (+0x1bc) is neither 0 nor 1.
  virtual bool IsInteractable() = 0;
  // 63  base false; MobileActor returns is_crouched (+0x187). Was CanBePickedUp, which is
  //     refuted by the override column: PickupActor carries the *base* (XOR AL,AL), so the
  //     one class that can be picked up always answered false. Only MobileActor and its
  //     descendants override it.
  virtual bool IsCrouched() = 0;
  // 64  Frag: scoring, splash, debris; broadcast 0x6b/0xba (0x37/0x38 for projectiles).
  virtual void Frag() = 0;
  // 65  set is_dead, run cleanup, and - **only if the argument is true** -
  //     broadcast 0x49. RET 0x4: the flag was missing from this declaration, so
  //     every call both drifted ESP and gated the broadcast on stack garbage.
  //     Every game call site passes 1.
  virtual void Delete(bool broadcast) = 0;
  // 66  base no-op; PickupActor stores the script name and broadcasts 0x84.
  virtual void Associate(char *script, char one_shot) = 0;
  // 67  base no-op; MobileActor broadcasts 0x97.
  virtual void Dissociate() = 0;
  // 68  damage/heal pipeline: absorb via armor/shield, frag at 0 strength.
  //
  // **Three stack arguments, not two.** Both implementations end in `RET 0xc`
  // and __thiscall is callee-clean, so declaring two made every call pop four
  // bytes more than the caller pushed - the ESP drift CLAUDE.md warns about,
  // which surfaces as an access violation with EIP on the stack, far from here.
  //
  // The third is the **attacker's team id**, or -1 for none.
  // `MobileActor::ApplyDamage` @ 0x00535ac0 uses it two ways: in Deathmatch it
  // credits the frag when it differs from the victim's team, and it is the
  // payload of the 0x9b update it broadcasts. `Actor::ApplyDamage` @ 0x0052f3b0
  // ignores it - the base is the rule for actors with no shield and no
  // networked health, and its replication comes from the `Frag` it ends in.
  virtual bool ApplyDamage(float, bool, int attacker_team) = 0;
  // 69  base no-op; health-changed hook.
  virtual void OnHealthChanged() = 0;
  // 70  the per-tick update, and the only vtable slot ExecutorActorTick
  //     @ 0x0052fad0 dispatches for work (displacement 0x118). Ten distinct
  //     bodies across the sixteen tables. The base @ 0x0052f8a0 advances a
  //     timer, stores the 64-bit clock into +0xc8/+0xd0 and `now` into +0xd8,
  //     pushes the transform to anim_object and only then, guarded on
  //     position_set_, broadcasts update 0x6f - a tick that opportunistically
  //     broadcasts, which is why it was named SyncPositionAndBroadcast here.
  //     Every mobile subclass chains to it and then does its whole per-tick
  //     logic (AI/movement, weapon fire, spline motion, projectile
  //     dead-reckoning).
  virtual void Update(int clock_lo, int clock_hi, float now_seconds) = 0;
  // 71  play an animation on anim_object (+0xe0).
  virtual void PlayAnimation(int, int, int, int, int, int) = 0;
  // 72  cross-fade blend into an animation.
  virtual void BlendAnimation(int, int, int, int, int, int, int) = 0;
  // 73  play an animation; arg 7 is the address of a completion flag to set.
  virtual void PlayAnimationEx(int, int, int, int, int, int, int, int) = 0;
  // 74  set the animation state machine on anim_object.
  virtual void SetAnimationState(unsigned int, int) = 0;
  // 75  base false; PickupActor returns has_associated_script (+0x148).
  virtual bool HasCustomAnimation() = 0;
  // 76  base no-op; TrackObjectActor installs a Catmull-Rom spline path.
  virtual void OnAnimationComplete(int, int, int) = 0; // RET 0xc: three arguments
  // 77  base no-op; TrackObjectActor starts a leg of motion, broadcast 0xad/0xae.
  virtual void OnAnimationEvent() = 0;
  // 78  set target, broadcast 0x56.
  virtual void SetTarget(int, int) = 0;
  // 79  clear target, broadcast 0x57.
  virtual void ClearTarget() = 0;
  // 80  reassign owner+team, broadcast 0x58 then 0x50.
  virtual void ChangeOwnerAndTeam(int, int, int) = 0;
  // 81  release from owner, broadcast 0x59 then 0x50.
  virtual void ReleaseFromOwner() = 0;
  // 82  (re)register in the spatial/team structures, set flag 0x200; MobileActor
  //     also disposes the inventory on death, BlockerActor un-blocks its nav polys.
  virtual void ActivateInWorld() = 0;
};
static_assert(sizeof(Actor) == 0x120);
static_assert(offsetof(Actor, vulnerabilities) == 0x10);
static_assert(offsetof(Actor, alarm_delay) == 0x30);
static_assert(offsetof(Actor, ai_type) == 0x50);
static_assert(offsetof(Actor, position) == 0xa0);
static_assert(offsetof(Actor, orientation) == 0xac);

// The actor's registration in the nav-mesh/pathfinding world, hanging off
// MobileActor::nav_agent - a nav-snapped position, a collision shape, the polygon
// it currently occupies, a velocity, and the traversal-capability flags the
// pathfinder tests against poly walkability. Exposed by slot 24 GetNavAgent
// (historically mis-named GetAIController: this is not an "AI brain").
//
// Built by CreateNavAgent (0x00472b80) from MobileActor::InitPositionAndTiming
// (slot 51) under TheMap->lock, which also allocates a 0x10-byte doubly-linked
// registration node (vtable @ 0x00652870, back-pointer at node+0xc) and threads it
// into the spatial-grid list. Torn down by DestroyNavAgent (0x00473e80): it unlinks
// that node, releases the shape via its vtable, and pool-frees these 0x34 bytes.
struct NavAgent {
  unsigned update_time;   // 0x00 game-clock stamp; min-tracked each tick by the Update
  Vec3 position;          // 0x04 world position, snapped onto the nav mesh
  // 0x10 collision-shape descriptor (pool_alloc 0x18, polymorphic; vtable
  // @ 0x006647a8). Released through its own vtable slot 0 by DestroyNavAgent.
  void *shape;
  // 0x14 the nav polygon the agent occupies (from FUN_0048d380). Its +0x14 is a
  // walkability bitfield: register sets 0x100040, unregister clears it.
  void *nav_poly;
  float radius;           // 0x18 collision radius = shape size field * DAT_00652874
  // 0x1c movement vector; the tick uses its squared length as the speed check.
  Vec3 velocity;
  float gravity;          // 0x28 per-agent gravity, default 9.81 (DAT_006a3a60)
  // 0x2c traversal-capability mask derived from the collision size (0x40100 base;
  // +0x400 tall, +0x80, +0x200 wide), then & 0xfffff97f. Read by MobileActor::SetTeamId.
  unsigned traversal_flags;
  unsigned traversal_flags_full; // 0x30 the same bits with 0x100000 forced on
};
static_assert(sizeof(NavAgent) == 0x34);
static_assert(offsetof(NavAgent, shape) == 0x10);
static_assert(offsetof(NavAgent, nav_poly) == 0x14);
static_assert(offsetof(NavAgent, velocity) == 0x1c);
static_assert(offsetof(NavAgent, traversal_flags) == 0x2c);

// The serialised motion state of one MobileActor, filled by
// MobileActor::WriteMotionSnapshot @ 0x0053bb00 (__thiscall(this, MotionSnapshot
// *out), RET 0x4). Every field is pinned by an instruction in that 122-byte
// body:
//
//   0053bb06  MOV EAX,[ECX + 0xd8]   -> time      (Actor::state_time)
//   0053bb0e  MOVQ  [ECX + 0xa0]     -> position.xy
//   0053bb1b  MOV EAX,[ECX + 0xa8]   -> position.z
//   0053bb24  MOVUPS [ECX + 0xac]    -> orientation, 16 bytes
//   0053bb35  MOVQ  [nav_agent+0x1c] -> velocity.xy
//   0053bb3f  MOV EAX,[nav_agent+0x24] -> velocity.z
//   0053bb55  MOV ECX,[nav_poly+0x20]  -> nav_poly_id, LOW 24 BITS ONLY
//
// When nav_agent->nav_poly is null those 24 bits are set to 0xffffff instead
// (0053bb66 `OR ECX,0xffffffff`); the top byte of that dword is preserved either
// way, which is why the id is a bitfield rather than a plain u32.
//
// This settles the payload layout of four wire updates, each of which builds
// {u32 id, u32 actor_id, ...} on the stack immediately below the snapshot:
// 0x39 (80 B) adds a Vec3 destination and a Vec3 current waypoint, 0x3b and 0x3c
// (56 B) carry the snapshot alone, and 0x5f (60 B) inserts an s32 speed delta
// before it.
struct MotionSnapshot {
  float time;         // 0x00
  Vec3 position;      // 0x04
  Vec4 orientation;   // 0x10
  Vec3 velocity;      // 0x20
  unsigned nav_poly_id : 24; // 0x2c low 24 bits; 0xffffff = no polygon
  unsigned field0x2f : 8;    // 0x2f the preserved top byte
};
static_assert(sizeof(MotionSnapshot) == 0x30);
static_assert(offsetof(MotionSnapshot, position) == 0x04);
static_assert(offsetof(MotionSnapshot, orientation) == 0x10);
static_assert(offsetof(MotionSnapshot, velocity) == 0x20);

struct MobileActor : Actor {
  char pad0x120[0x40];
  Character *character;        // 0x160 cached role->character
  char pad0x164[4];
  float sight_range;           // 0x168
  float initial_first_person_range;
  float alert_radius;          // 0x170
  float sight_angle;           // 0x174
  int walking_speed;           // 0x178
  float field0x17c;            // 0x17c slot 60 compares this against a global
  char pad0x180[4];
  bool is_moving;              // 0x184 slot 14
  char field0x185;
  bool is_concealed;           // 0x186 slots 8/9  (was is_mine)
  bool is_crouched;            // 0x187 slot 63    (was can_be_picked_up)
  bool field0x188;             // 0x188 slots 30/54
  char pad0x189[3];
  int field0x18c;              // 0x18c slots 25/26; used as a team-slot index
  short field0x190;            // 0x190 anim channel (lo) + busy flag (hi)
  char pad0x192[2];
  pool_unique_ptr<Inventory> inventory; // 0x194 slot 16; 0x44-byte container
  void *field0x198;            // 0x198 slot 92; Hierarchy* override
  // 0x19c slot 34 returns &inventory_list. Entries are 0x18 bytes and pool-owned:
  // ~MobileActor pool-frees each one as it drains the list.
  List<void *> inventory_list;
  char pad0x1ac[8];            // 0x1ac
  float aim;                   // 0x1b4
  char pad0x1b8[4];
  int move_state;              // 0x1bc slot 62; MobileActor::SetMoveState
  void *dest_node;             // 0x1c0 nav node (was `path_handle`)
  short waypoint_active;       // 0x1c4
  char pad0x1c6[6];
  void *path_object;           // 0x1cc ref-counted; slot 52
  Vec3 collision_bounds;       // 0x1d0
  Vec3 goto_target;            // 0x1dc
  // 0x1e8 / 0x1ec are **game-time seconds, not priorities**. Every writer stores
  // the tick's `now`: GotoObject @ 0x005394d0 sets both to it, 0x00539540 sets
  // +0x1ec = Actor::state_time, and CancelLastOrder / Dissociate /
  // ReleaseFromOwner all call GetGameTimeSeconds @ 0x00571b10 immediately before
  // the call that writes +0x1e8. The gate `+0x1e8 <= now` guarding GotoObject
  // and StopAndBroadcast is therefore a stale-command / re-entry guard, not an
  // arbitration between competing orders - which is what `goto_priority` and
  // `goto_priority_current` implied here until it was measured.
  float move_cmd_time;         // 0x1e8
  float move_start_time;       // 0x1ec
  List<void *> order_queue;    // 0x1f0 (size() @ 0x1f4 is slot 32)
  NavAgent *nav_agent;         // 0x200 slot 24 (see NavAgent above)
  // 0x204 the *route* list - the waypoints the navigation solve produces.
  // PushRouteWaypoint @ 0x0053a640 push-FRONTs a pool_alloc'd 0x18-byte
  // {Vec3 pos; u32; u32} record onto it (which is what makes the A* backtrack
  // come out in travel order), ClearRouteWaypoints @ 0x0053a450 drains it.
  List<void *> waypoints;      // 0x204
  // 0x214 a SECOND List<void *>, of authored waypoints. MobileActor::Ctor
  // initialises it as a list at 0x005327b4-0x005327da, exactly as it does the
  // one above; AppendAuthoredWaypoint @ 0x0053a830 push-BACKs onto it and
  // ClearAuthoredWaypoints @ 0x0053a550 drains it. Slot 90 AddWaypoint pushes to
  // both. Nothing in the binary reads an element's payload - the only read is
  // `CMP [EBX+0x218],0`, i.e. the count, used by AiThink_Bot as a "have I
  // recorded anything yet" latch. This was `void *field0x214` plus 0xc of
  // padding here until the constructor was read.
  List<void *> waypoints_authored; // 0x214
  void *waypoint_ptr;          // 0x224 cursor into the route list
  // 0x228 -> &waypoints, confirmed by `MOV [EDI+0x228],ESI` in the constructor
  // with ESI = EDI + 0x204. It points at the route list, never at the authored
  // one.
  List<void *> *waypoint_list; // 0x228
  char pad0x22c[4];

  // MobileActor extension slots 83-94.
  // 83  @ 0x00536090, bare RET. Flips is_crouched (+0x187) and sets/clears
  //     is_concealed (+0x186) from a cover test and nav-polygon flag 0x800 (the
  //     water/camouflage-terrain bit, and the binary's only reader of it - see
  //     stealth_and_fog_notes.md §2). Also swaps the collision box between
  //     standing and halved, and broadcasts `0x4c + 2*(!is_crouched)`, i.e. 0x4c
  //     for crouched and 0x4e for standing; gated on model node 0x13. Nothing in
  //     it detects mines - it was named UpdateMineDetectionAndBounds here, and
  //     flagged doubtful, until it was read.
  virtual void ToggleCrouchAndCamouflage() = 0;
  // 84  equip into the first free slota..sloth (inventory-list indices 2-9).
  virtual void EquipToFirstOpenSlot(int, int) = 0;
  // 85  append a tag-10 order record (0x28 bytes) to the order queue (+0x1f0).
  virtual void QueueOrderKind10(int) = 0;
  // 86  append a tag-1 order record carrying a Vec3 (kind at +0x00, the Vec3 at
  // +0x10, the flag at +0x20). The Vec3 is passed **by value**, which RET 0x10
  // cannot tell you - a Vec3* plus three dwords is the same 16 bytes. What
  // settles it is the callee: MobileActor's own stub @ 0x0054e5f0 consists of a
  // single `_eh_vector_destructor_iterator_(&[EBP+8], 4, 3, ...)`, which MSVC
  // emits only to destroy a by-value class parameter. CharacterActor's override
  // @ 0x00541ac0 then reads x,y from [EBP+8], z from [EBP+0x10] and the flag
  // from [EBP+0x14]. Declared here as (Vec3*, int, char, int) until measured.
  virtual void QueueOrderPosition(Vec3, bool) = 0;
  // 87  append a tag-0 order record.
  virtual void QueueOrderTarget(int, char) = 0;
  // 88  issue a move order, gated on can_turn, strength and priority.
  virtual int Goto(Vec3 *, float) = 0;
  // 89  death: broadcast 0x3d (destructible) or 0x48, then slots 82 and 64.
  virtual void Die() = 0;
  // 90  push a 0x18-byte waypoint record onto the waypoint list (+0x204).
  // Despite also being RET 0x10, this is *not* slot 86's signature: the body
  // dereferences its first argument (`MOV ECX,[EBP+8]` then `MOVQ XMM0,[ECX]`)
  // and has no vector destructor, and MobileActor::PathToTarget @ 0x00539930
  // pushes the pointer a position getter just returned. The last argument is a
  // float, not an int - the one in-binary caller pushes 0.0f. Arguments 2 and 4
  // land at record+0x0c and +0x10; that caller passes 0 for both, so what they
  // mean is not established.
  virtual void AddWaypoint(Vec3 *, int, char, float) = 0;
  // 91  fill a 0x24-byte navigation-target descriptor.
  virtual void GetNavigationTarget(int *) = 0;
  // 92  return the Hierarchy* override at +0x198.
  virtual void *GetField0x198() = 0;
  // 93  play an action animation, gated on the busy flag and a timestamp (+0x88).
  virtual void PlayActionAnimation(int, float) = 0;
  // 94  base setter; CharacterActor frees the old 0x28 weapon and builds a new one
  //     (0x21 = none), broadcast 0x83.
  virtual void SetWeapon(int) = 0;
};
static_assert(sizeof(MobileActor) == 0x230);
static_assert(offsetof(MobileActor, character) == 0x160);
static_assert(offsetof(MobileActor, is_concealed) == 0x186);
static_assert(offsetof(MobileActor, inventory) == 0x194);
static_assert(offsetof(MobileActor, nav_agent) == 0x200);
static_assert(offsetof(MobileActor, move_cmd_time) == 0x1e8);
// The two waypoint lists. These three are what pin the second List<void *> at
// 0x214 - without them the 0x230 size assert alone would pass for the old
// `void *field0x214` + `char pad0x218[0xc]` spelling too.
static_assert(offsetof(MobileActor, waypoints) == 0x204);
static_assert(offsetof(MobileActor, waypoints_authored) == 0x214);
static_assert(offsetof(MobileActor, waypoint_ptr) == 0x224);
static_assert(offsetof(MobileActor, waypoint_list) == 0x228);

// Records every nav-mesh polygon whose "blocked" bit (0x100) this actor set, so
// slot 82 can undo them.
struct BlockerActor : Actor {
  List<void *> blocked_polys; // 0x120
};
static_assert(sizeof(BlockerActor) == 0x130);
static_assert(offsetof(BlockerActor, blocked_polys) == 0x120);

struct PickupActor : Actor {
  int enabled;              // 0x120 slot 13
  int action_on_death;      // 0x124
  int pickup_type;          // 0x128 == role->character->aggression * 10
  char is_destructible;     // 0x12c gates GetArmorValue and SetPickupEnabled
  char pad0x12d[3];
  char pad0x130[4];
  pool_string associated_script; // 0x134 set by slot 66 Associate
  // 0x138 the REQUIRES console command's item: the name of a Role the collector
  // must already hold in inventory before this actor can be picked up/activated
  // (`REQUIRES DOOR_A KEY_1`). Set by slot 85 SetRequiredItem; read by slot 84
  // OnPickedUp, which denies the pickup unless the inventory contains a matching
  // role. NOT associated_script (+0x134), which is the ASSOCIATE script path.
  pool_string required_item_name; // 0x138
  bool is_script_oneshot;   // 0x13c
  char pad0x13d[3];
  int respawn_delay_mode;   // 0x140 1 -> x2, 2 -> x1, 3 -> x0.5
  float respawn_at_time;    // 0x144 game-time deadline (was `max_distance`)
  int has_associated_script; // 0x148 slot 75; set by slot 66 Associate
  float pickup_radius;      // 0x14c

  // PickupActor extension slots 83-85.
  // 83  store enabled (+0x120); when pickup_type == 0, schedule respawn_at_time
  //     (MPRespawnDelay scaled by respawn_delay_mode). Broadcast 0x85.
  virtual void SetPickupEnabled(bool) = 0;
  // 84  collected: broadcast 0x74/0x75/0x8c/0x4f, and run associated_script if set.
  virtual void OnPickedUp(MobileActor *) = 0;
  // 85  free and strdup the REQUIRES item name into required_item_name (+0x138);
  //     NOT associated_script (+0x134).
  virtual void SetRequiredItem(const char *) = 0;
};
static_assert(sizeof(PickupActor) == 0x150);
static_assert(offsetof(PickupActor, associated_script) == 0x134);
static_assert(offsetof(PickupActor, required_item_name) == 0x138);
static_assert(offsetof(PickupActor, respawn_at_time) == 0x144);

// Launch origin + velocity + launch time integrated against gravity, with the role's
// `projectile` sub-object supplying damage, splash and radius. Slot 51 precomputes the
// impact, slot 55 is the physics step, slot 70 the dead-reckoning (which despite the
// slot name broadcasts nothing itself). Called `UnknownActor` in older notes.
struct ProjectileActor : Actor {
  Vec3 initial_position;  // 0x120 launch anchor for the ballistic closed form
  int owner_actor_id;     // 0x12c skipped by every hit test
  float damage_scale;     // 0x130 scales damage, not velocity
  void *projectile;       // 0x134 Role::projectile
  Vec3 velocity;          // 0x138
  char pad0x144[0xc];     // 0x144 launch time, 0x148/0x14c launch tick (int64)
  unsigned flags;         // 0x150 slot 83; 0x10 gore, 0x40 guided, 0x200 dissociate
  bool hit_predicted;     // 0x154
  bool hit_is_world;      // 0x155
  char pad0x156[2];
  float time_to_impact;   // 0x158
  int hit_actor_id;       // 0x15c
  void *weapon_context;   // 0x160
  void *target_actor;     // 0x164
  Vec3 target_position;   // 0x168 written by slot 84
  int field0x174;

  // ProjectileActor extension slots 83-84.
  // 83  return the +0x150 bitfield (0x10 gore, 0x40 guided, 0x200 dissociate).
  virtual unsigned GetProjectileFlags() = 0;
  // 84  write a Vec3 by value into +0x168, the guidance/arrival target.
  virtual void SetTargetPosition(Vec3) = 0;
};
static_assert(sizeof(ProjectileActor) == 0x178);
static_assert(offsetof(ProjectileActor, flags) == 0x150);
static_assert(offsetof(ProjectileActor, target_position) == 0x168);

struct TrackObjectActor : Actor {
  bool affects_map;       // 0x120
  bool path_valid;        // 0x121
  bool moving;            // 0x122
  bool reverse;           // 0x123 selects message 0xad / 0xae
  bool cycle_enabled;     // 0x124
  char pad0x125[3];
  unsigned cycle_ticks;   // 0x128
  float inv_duration;     // 0x12c elapsed * this = spline parameter t
  void *geometry;         // 0x130 entry from the Map+0x24 track list
  float coeffs[3][4];     // 0x134 cubic [x|y|z][t^3, t^2, t, 1]
  char pad0x164[0x14];
  Vec4 start_ori;         // 0x178
  Vec4 target_ori;        // 0x188
  char pad0x198[8];       // 0x198 int64 start time
  char pad0x1a0[8];
  List<Actor *> riders;   // 0x1a8 refcounted Actors being carried
};
static_assert(sizeof(TrackObjectActor) == 0x1b8);
static_assert(offsetof(TrackObjectActor, geometry) == 0x130);
static_assert(offsetof(TrackObjectActor, riders) == 0x1a8);

struct TumbleweedActor : Actor {};
static_assert(sizeof(TumbleweedActor) == 0x120);

struct BackgroundCreatureActor : Actor {};
static_assert(sizeof(BackgroundCreatureActor) == 0x120);

struct CharacterActor : MobileActor {
  char pad0x230[0x28];
  float weapon_cycle_time;    // 0x258
  float weapon_cycle_offset;  // 0x25c
  char pad0x260[1];
  char attack_mode_param;     // 0x261
  char pad0x262[0x26];
  char close_range_attack;    // 0x288
  char attack_active_flag;    // 0x289
  char pad0x28a[6];
  int attack_range;           // 0x290
  char pad0x294[0x10];
  int weapon_type;            // 0x2a4
  char pad0x2a8[0x10];
  pool_unique_ptr<Weapon> weapon; // 0x2b8 slot 11; pool-freed by ~CharacterActor
  Vec3 aim_direction;         // 0x2bc (TurretActor uses it as muzzle position)
  void *hotspot;              // 0x2c8 slot 31
  void *alternate_hotspot;    // 0x2cc
  float shield_value;         // 0x2d0 slots 19/29
  char is_attacking;          // 0x2d4 slot 7
  char pad0x2d5[3];
  void *attack_target;        // 0x2d8 slot 15
  Vec3 attack_position;       // 0x2dc
  char pad0x2e8[4];           // 0x2e8..0x2ea animation-complete flags
  float weapon_range;         // 0x2ec (TurretActor uses it as muzzle speed)
  char pad0x2f0[8];           // 0x2f0/0x2f4 gun yaw/pitch, float despite the int typing
  int attack_stop_reason;     // 0x2f8
  float target_cycle_delay;   // 0x2fc
  void *selected_ammo;        // 0x300
  int field0x304;             // 0x304 slots 23/95; cannot-fire gate

  // CharacterActor extension slots 95-99.
  // 95  write the cannot-fire gate (+0x304). Setter for slot 23.
  virtual void SetField0x304(int) = 0;
  // 96  attack an actor; broadcast id is computed 0x41 + close_range.
  virtual void AttackTarget(Actor *, int, char, char) = 0;
  // 97  attack a position; broadcast id is computed 0x3f + close_range.
  virtual void AttackPosition(Vec3 *, int, char, char) = 0;
  // 98  stop attacking, broadcast 0x44.
  virtual void StopAttacking(int) = 0;
  // 99  set the weapon's ammo type, reselect ammo, broadcast 0x82.
  virtual void SetAmmoType(int) = 0;
};
static_assert(sizeof(CharacterActor) == 0x308);
static_assert(offsetof(CharacterActor, weapon) == 0x2b8);
static_assert(offsetof(CharacterActor, is_attacking) == 0x2d4);
static_assert(offsetof(CharacterActor, attack_target) == 0x2d8);
static_assert(offsetof(CharacterActor, field0x304) == 0x304);

struct NodeActor : MobileActor {
  char pad0x230[8];    // 0x230/0x234 int64 spawn timestamp
  char pad0x238[0x20];
  // 0x258 each payload is itself a list of 0x18-byte waypoint records, which
  // ~NodeActor pool-frees.
  List<void *> paths;
  void *current_path;  // 0x268 slot 90 appends to *(current_path + 0xc)
  char pad0x26c[0xc];
};
static_assert(sizeof(NodeActor) == 0x278);
static_assert(offsetof(NodeActor, paths) == 0x258);
static_assert(offsetof(NodeActor, current_path) == 0x268);

// The escort/VIP actor: it walks to an `exita`..`exitd` map aux object and, on
// arrival, broadcasts 0x9b and deletes itself. It also defects to whichever team
// has the nearest actor.
struct PresidentActor : MobileActor {
  Vec3 exit_position;          // 0x230 slot 95
  float last_team_switch_time; // 0x23c
  // PresidentActor extension slot 95.
  // 95  write the exit position by value into +0x230..0x23b.
  virtual void SetExitPosition(Vec3) = 0;
};
static_assert(sizeof(PresidentActor) == 0x240);
static_assert(offsetof(PresidentActor, exit_position) == 0x230);

struct FlyingBackgroundCreatureActor : BackgroundCreatureActor {};
static_assert(sizeof(FlyingBackgroundCreatureActor) == 0x120);

struct CentibodyActor : CharacterActor {
  Actor *next_segment; // 0x308 refcounted; the follow-the-leader body chain
  char pad0x30c[4];
};
static_assert(sizeof(CentibodyActor) == 0x310);
static_assert(offsetof(CentibodyActor, next_segment) == 0x308);

struct PopupActor : CharacterActor {
  bool deployed;                // 0x308
  bool transition_in_progress;  // 0x309
  char pad0x30a[6];
};
static_assert(sizeof(PopupActor) == 0x310);
static_assert(offsetof(PopupActor, deployed) == 0x308);

struct CentipedeActor : CentibodyActor {};
static_assert(sizeof(CentipedeActor) == 0x310);

struct TurretActor : PopupActor {
  bool turret_enabled;      // 0x310 slots 100/103
  char pad0x311[7];         // 0x311 alignment; never read/written (ctor skips it)
  // 0x318/0x31c target yaw/pitch: written separately by slot 104, read together as
  // an int64 pair by slot 102. Despite the int typing they hold packed angles.
  int target_angle_yaw;     // 0x318 slots 102/104
  int target_angle_pitch;   // 0x31c
  // TurretActor extension slots 100-104. Note the firing solution (slot 70)
  // integrates CharacterActor+0x2f0/+0x2f4 (gun yaw/pitch), not these fields.
  // 100  store turret_enabled (+0x310).
  virtual void SetTurretEnabled(bool) = 0;
  // 101  out-param the aim direction (+0x2bc).
  virtual void GetTurretAimDirection(long long *) = 0;
  // 102  return the packed target_angle_yaw/pitch pair (+0x318/+0x31c).
  virtual long long GetTurretTargetAngles() = 0;
  // 103  read turret_enabled.
  virtual bool IsTurretEnabled() = 0;
  // 104  set target_angle_yaw/pitch.
  virtual void SetTurretTargetAngles(int, int) = 0;
};
static_assert(sizeof(TurretActor) == 0x320);
static_assert(offsetof(TurretActor, turret_enabled) == 0x310);
static_assert(offsetof(TurretActor, target_angle_yaw) == 0x318);

// The actor hash @ 0x007ba0d8. Unlike the roles table this one is a real object:
// its address is passed as `this` to the template's own methods, and 0x0054f2b0 is
// a byte-for-byte match for AvP's `_base_HashTable::Remove` (chains @ this+0x10,
// mask @ this+0x0c, n_entries @ this+0x04, `free_sized(node, 8)` for the node), so
// the field at +0x00 that older notes called `unk1` is the vptr.
using Actors = HashTable<Actor *>;
using ActorNode = Actors::Node;
static_assert(sizeof(Actors) == 0x14);
static_assert(sizeof(ActorNode) == 0x8);
// These are the layout proof: they fail if the vptr does not land first, pushing
// the inherited fields to the offsets GetActorById reads.
static_assert(offsetof(Actors, n_entries) == 0x04);
static_assert(offsetof(Actors, table_size_mask) == 0x0c);
static_assert(offsetof(Actors, chains) == 0x10);

// --- Native API over the game's actor table ---------------------------------

// The global actor hash @ 0x007ba0d8. Iterate with begin()/end().
Actors *GetActorsTable();

// GetActorById @ 0x0044e0b0; returns nullptr for an unknown id.
Actor *GetActorById(int id);
} // namespace gk
