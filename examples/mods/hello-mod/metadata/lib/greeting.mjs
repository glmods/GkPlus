// A helper a mod's entry module imports. It exists to show that it *can* be
// imported: every `.mjs` under `metadata/` is read when the mod loads, so an
// ordinary relative specifier resolves inside a `.zip` as well as inside a
// directory, where neither file has a path on disk for the host to read.
//
// `"gk"` is importable from here too - it is a bare specifier, so it resolves to
// the module the host registered rather than to a file.

/**
 * @param {string} name
 * @returns {string}
 */
export function greeting(name) {
  return `Hello from ${name}`;
}
