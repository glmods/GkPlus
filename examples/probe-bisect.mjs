// Works out WHY a generated GLS section fails to parse, by feeding the real
// parser a battery of one-line variants and reporting which survive.
//
// Install as <Gunlok>\gkplus\main.mjs and start the game; results go to the
// in-game console (`) and the debugger, prefixed [gkplus].
//
// `gls.try_parse(text)` returns how many objects reached ParsedObjList, or -1
// when none did (syntax error, or every section demoted to abstract because a
// required field was missing - the parser does not distinguish those to us).
//
// Reading the results: `1` means the section parsed AND was complete. `-1` means
// it did not. The point of each pair below is that only one thing differs.

import { console, gls } from "gk";

/** @param {string} label @param {string} source */
function t(label, source) {
  let n;
  try {
    n = gls.try_parse(source);
  } catch (e) {
    n = `threw: ${e instanceof Error ? e.message : String(e)}`;
  }
  console.log(`  ${String(n).padStart(3)}  ${label}`);
}

console.log("=== GLS parse bisection ===");

// 1. Is it the label? destructibility is the cleanest case - one field, no filler,
//    and structurally identical to the shipped `destructibility Des_Explode`.
console.log("--- destructibility: label shapes ---");
t("shipped label   Des_Explode", "destructibility Des_Explode\n{\n\ttype explode\n}\n");
t("our label       GkPlusProbe", "destructibility GkPlusProbe\n{\n\ttype explode\n}\n");
t("underscored     Gk_Probe", "destructibility Gk_Probe\n{\n\ttype explode\n}\n");
t("lowercase       probe", "destructibility probe\n{\n\ttype explode\n}\n");
t("no label", "destructibility\n{\n\ttype explode\n}\n");
t("braces same line", "destructibility Des_Explode { type explode }\n");

// 2. Is it the section keyword or the field? Same label, different sections.
console.log("--- section keyword with a known-good body ---");
t("ammo info (worked before)", "ammo info GkPlusProbe\n{\n\tammo type flares\n}\n");
t("destructibility", "destructibility GkPlusProbe\n{\n\ttype explode\n}\n");
t("light", "light GkPlusProbe\n{\n\tred 1 green 1 blue 1 range 10\n}\n");

// 3. Are the suspect field spellings real? The parser reports `nolighting` and
//    `generategenerators`, but scripts may spell them with a space. Each pair
//    differs only in that.
console.log("--- field spellings: message name vs script keyword ---");
t("role: nolighting", "role GkPlusProbe\n{\n\tai bot\n\tnolighting no\n}\n");
t("role: no lighting", "role GkPlusProbe\n{\n\tai bot\n\tno lighting no\n}\n");
t("pgen: generategenerators", "pgenerator GkPlusProbe\n{\n\ttype smoke\n\tgenerategenerators no\n}\n");
t("pgen: generate generators", "pgenerator GkPlusProbe\n{\n\ttype smoke\n\tgenerate generators no\n}\n");

// 4. The minimal role - does `ai bot` alone parse, with nothing else at all?
//    If this is -1 while a shipped role parses, the difference is completeness,
//    not syntax.
console.log("--- role: minimal vs realistic ---");
t("role: ai only", "role GkPlusProbe\n{\n\tai bot\n}\n");
t("role: abstract, ai only", "abstract role GkPlusProbe\n{\n\tai bot\n}\n");
t("role: ai + identifier", 'role GkPlusProbe\n{\n\tai bot\n\tidentifier "probe"\n}\n');
t(
  "role: shipped Rol_DefaultRobot body",
  "role GkPlusProbe\n{\n\tlight none\n\tprojectile none\n\tidentifier none\n" +
    "\tper vertex fogging no\n\talpha fogging yes\n\tdestructibility none\n\treflective yes\n}\n"
);

// 5. pgenerator needs `life`, which reflection cannot see (field id 0x42 bypasses
//    CheckValue into the section's 0x1b70 extension). Shipped scripts write
//    `life infinite` or `life 5 seconds`.
console.log("--- pgenerator: the invisible `life` field ---");
t("pgen: no life", "pgenerator GkPlusProbe\n{\n\ttype smoke\n\trate 4\n\tred 0.1 green 0.1 blue 0.1 alpha 0.7\n\tx 0 y 0 z 0\n}\n");
t("pgen: life infinite", "pgenerator GkPlusProbe\n{\n\ttype smoke\n\tlife infinite\n\trate 4\n\tred 0.1 green 0.1 blue 0.1 alpha 0.7\n\tx 0 y 0 z 0\n}\n");
t("pgen: shipped Pgn_Adversor body", "pgenerator GkPlusProbe\n{\n\ttype fire\n\tlife infinite\n\trate 4\n\tx 0 y 0 z 0\n\tred 0.1 green 0.1 blue 0.1 alpha 0.7\n}\n");

// 6. character - which filler line breaks it? `weapon cycle time2` has a digit,
//    and `status window u` is three words.
console.log("--- character: suspect field spellings ---");
t("char: minimal", "character GkPlusProbe\n{\n\tweapon none\n}\n");
t("char: + cycle time2", "character GkPlusProbe\n{\n\tweapon none\n\tweapon cycle time2 0\n}\n");
t("char: + status window u", "character GkPlusProbe\n{\n\tweapon none\n\tstatus window u 0\n}\n");
t("char: + generation limit", "character GkPlusProbe\n{\n\tweapon none\n\tgeneration limit 1\n}\n");
t("char: shipped Chr_Default body", "character GkPlusProbe\n{\n\twalking speed 1\n\tturning speed 0.5\n\taggression 0.7\n\tsight range 22\n\tsight angle 45\n\thearing range 17\n}\n");

console.log("=== bisection done ===");
