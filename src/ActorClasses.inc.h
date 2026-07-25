// X-macro list of the Actor class hierarchy, mirroring src/Actors.h and the
// tree in actor_vtable_notes.md. Included by src/JsActors.cpp to generate the
// per-class JSClassIDs, the class defs, the RTTI dispatch ladder, the `kind`
// strings and the prototype chain.
//
//   GK_ACTOR_CLASS(Name, Parent, Predicate, Kind)
//
//     Name       the struct in Actors.h, and the JS class name
//     Parent     its immediate base
//     Predicate  the Actor slot 36-50 test that identifies it
//     Kind       what `actor.kind` reports
//
// The root `Actor` is deliberately absent: it has no RTTI predicate of its own
// and is the fallback in every generated dispatch.
//
// ORDER MATTERS, and one rule covers both uses: **every class is listed before
// its own base**. The predicates are inherited - IsMobile() is true for a turret
// too - so the dispatch ladder must test most-derived first; and the prototype
// chain is built by walking this list backwards, which needs each base's
// prototype to exist already. JsActors.cpp static_asserts the rule.

GK_ACTOR_CLASS(CentipedeActor, CentibodyActor, IsCentipede, "centipede")
GK_ACTOR_CLASS(CentibodyActor, CharacterActor, IsCentibody, "centibody")
GK_ACTOR_CLASS(TurretActor, PopupActor, IsTurret, "turret")
GK_ACTOR_CLASS(PopupActor, CharacterActor, IsPopup, "popup")
GK_ACTOR_CLASS(CharacterActor, MobileActor, IsCharacter, "character")
GK_ACTOR_CLASS(NodeActor, MobileActor, IsNode, "node")
GK_ACTOR_CLASS(PresidentActor, MobileActor, IsPresident, "president")
GK_ACTOR_CLASS(MobileActor, Actor, IsMobile, "mobile")
GK_ACTOR_CLASS(FlyingBackgroundCreatureActor, BackgroundCreatureActor,
               IsFlyingBackgroundCreature, "flying_background_creature")
GK_ACTOR_CLASS(BackgroundCreatureActor, Actor, IsBackgroundCreature,
               "background_creature")
GK_ACTOR_CLASS(BlockerActor, Actor, IsBlocker, "blocker")
GK_ACTOR_CLASS(PickupActor, Actor, IsPickup, "pickup")
GK_ACTOR_CLASS(ProjectileActor, Actor, IsProjectile, "projectile")
GK_ACTOR_CLASS(TrackObjectActor, Actor, IsTrackObject, "track_object")
GK_ACTOR_CLASS(TumbleweedActor, Actor, IsTumbleweed, "tumbleweed")
