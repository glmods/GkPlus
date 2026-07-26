// Scripts\bug.gsh, re-implemented as a module.
//
// Nothing here is parsed and nothing builds a ParsedThing: `make` calls the
// native constructors in src/MakeRole.cpp, which are the game's own ToXxx
// converters re-expressed over plain description structs. A definition costs a
// few dozen bytes instead of the 0x1b60 a parsed object does, and the lexer, the
// yacc grammar and the #include preprocessor never run at all.
//
// The translation from GLS is close to mechanical:
//
//   bug.gsh                              bug.mjs
//   ------------------------------------ -----------------------------------
//   hierarchy Hcy_Bug { file, name }     hierarchy: { rif, object }
//   character Chr_Bug : Chr_DefaultBaddie   ...defaults.Chr_DefaultBaddie
//   walking speed 1.5                    walking_speed: 1.5
//   draw vision cone no                  draw_vision_cone: false
//   ai background creature               ai: "background creature"
//   character none                       (omit it)
//   destructibility Des_Explode          destructibility: { kind: "explode" }
//
// Two differences worth knowing:
//
//  * A role owns its character, light, projectile, pgens and destructibility -
//    RoleDtor pool-frees all six - so they are described *inline* rather than
//    built separately and shared. Handing one object to two roles would
//    double-free it on level teardown; this makes that unrepresentable.
//  * GLS inheritance (`child : parent`) has no equivalent, because there is no
//    parsed object to copy fields from. Object spread does the same job, and
//    does it at authoring time rather than inside the game.
//
// Values are in the same units a .gsh literal is - degrees, seconds, metres,
// animation cycles per second. The conversion to BAM angles and 16.16 fixed
// point happens inside the constructor, so a number here means what the same
// number means in the original file.

import { make } from "gk";
import { Chr_DefaultBaddie, Rol_DefaultRobot } from "./defaults.mjs";

// H I E R A R C H I E S ///////////////////////////////////////////////////////

/** @type {import("gk").AssetRef} */
export const Hcy_Bug = {
  rif: "units\\bug.rif",
  object: "bug",
  hotspot: "head",
};

// C H A R A C T E R S /////////////////////////////////////////////////////////

/** @type {import("gk").CharacterDesc} */
export const Chr_Bug = {
  ...Chr_DefaultBaddie,
  turning_speed: 0.4,
  walking_speed: 1.5,
  strength: 1,
  aim: 20,
  sight_angle: 70,
  sight_range: 20,
  hearing_range: 25,
  aggression: 0.1,
};

/** @type {import("gk").CharacterDesc} */
export const Chr_SuperBug = {
  ...Chr_DefaultBaddie,
  turning_speed: 0.4,
  walking_speed: 10,
  strength: 1000,
  aim: 0,
  sight_angle: 89,
  sight_range: 40,
  hearing_range: 45,
  aggression: 0.9,
  size: 15,
  weapon: "enemy missile launcher plus",
  draw_vision_cone: false,
  draw_hearing_range: false,
};

// R O L E S ///////////////////////////////////////////////////////////////////
//
// Roles are *registered* as they are made, and the roles hash is cleared between
// levels - so these are functions, called from a level's `define` hook once per
// load, not values built at module scope.

/** @returns {import("gk").Role} */
export function Rol_Bug() {
  return make.role({
    ...Rol_DefaultRobot,
    hierarchy: Hcy_Bug,
    character: Chr_Bug,
    identifier: "bug",
    ai: "background creature",
    reflective: false,
  });
}

/** @returns {import("gk").Role} */
export function Rol_SuperBug() {
  return make.role({
    ...Rol_DefaultRobot,
    hierarchy: Hcy_Bug,
    character: Chr_SuperBug,
    identifier: "SuperBug",
    ai: "bot",
    destructibility: { kind: "explode" },
    status_display: false,
  });
}

/** Everything this module contributes, for a level's `define` to register. */
export const roles = [Rol_Bug, Rol_SuperBug];
