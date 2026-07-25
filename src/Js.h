#pragma once

#include <quickjs.h>

namespace gk::js {
// Registers the "gk" C module in `ctx`. Call once per context, before evaluating
// any script that imports it; returns false if any registration step failed.
//
// The module exports six live namespace objects - `camera`, `console`, `actors`,
// `roles`, `tokens`, `triggers` - both as named exports and as properties of the
// default export, which are the same objects:
//
//   import gk, { actors, camera } from "gk";
//   gk.actors === actors;          // true
//   camera.distance = 900;
//
// This registers the bindings and nothing else. Creating the runtime/context,
// loading scripts and pumping the job queue are a host's job, and no host exists
// yet - so until one lands, nothing in the process ever imports "gk".
bool RegisterGkModule(JSContext *ctx);
} // namespace gk::js
