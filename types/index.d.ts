// Entry point for the npm package. Neither file below exports anything a
// bundler could import - gk.d.ts is an ambient `declare module "gk"` and
// imgui.d.ts is a global `ImGui` type - so this just pulls both into scope
// for anyone who depends on the package. Referencing it is enough:
//
//   /// <reference types="@glmods/gkplus-types" />
//
// or, in a tsconfig/jsconfig that already resolves node_modules:
//
//   { "compilerOptions": { "types": ["@glmods/gkplus-types"] } }
//
/// <reference path="./gk.d.ts" />
/// <reference path="./imgui.d.ts" />
