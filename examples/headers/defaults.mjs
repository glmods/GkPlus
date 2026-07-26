// The part of Scripts\defaults.gsh that examples\headers\bug.mjs needs, as a
// worked example of translating a .gsh header. The real defaults.gsh is far
// larger and ends with a dozen #includes; this covers the two symbols bug
// imports and their base.
//
// GLS inheritance (`abstract character Chr_Default` then
// `character Chr_DefaultBaddie : Chr_Default`) becomes object spread. That is
// not a workaround - it is strictly better placed. In GLS, `copyFields` runs
// inside the game and copies the parent's whole value set, which is why `extends`
// had to be applied before the child's own fields; here the merge happens at
// authoring time and JS's own ordering rules say what wins.
//
// `abstract` needs no equivalent either. It existed to suppress the
// "incomplete object definition" warning for a base that deliberately leaves
// required fields unset - but a description is only a plain object until
// something calls make.role on it, so an incomplete one is simply never built.

/** @type {import("gk").CharacterDesc} */
export const Chr_Default = {
  walking_speed: 1, // animation cycles per second
  turning_speed: 0.5, // revolutions per second
  aggression: 0.7, // between 0 and 1
  sight_range: 22, // metres
  sight_angle: 45, // degrees left or right
  hearing_range: 17, // metres
};

/** @type {import("gk").CharacterDesc} */
export const Chr_DefaultGoodie = { ...Chr_Default, strength: 20, aim: 1 };

/** @type {import("gk").CharacterDesc} */
export const Chr_DefaultBaddie = { ...Chr_Default, strength: 10, aim: 4 };

/**
 * role Rol_DefaultRobot - the shared tail of every robot role.
 *
 * The GLS original writes `light none` / `projectile none` / `identifier none`
 * explicitly; here they are simply absent, which is the same thing. The three
 * flags are not defaults, though, so they have to be stated: `alpha fogging yes`
 * wins over `per vertex fogging`, which the constructor forces off.
 *
 * @type {Partial<import("gk").RoleDesc>}
 */
export const Rol_DefaultRobot = {
  per_vertex_fogging: false,
  alpha_fogging: true,
  reflective: true,
};
