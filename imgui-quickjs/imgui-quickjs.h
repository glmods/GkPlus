#pragma once

#include <quickjs.h>

#ifdef __cplusplus
extern "C" {
#endif

// A fresh object carrying the whole ImGui surface - the value the script host
// passes to `draw_gui`. Not a module: these calls are only valid inside an
// active ImGui frame, so the object is scoped to the callback that runs in one
// rather than being importable from anywhere.
JSValue js_imgui_new_namespace(JSContext *ctx);

#ifdef __cplusplus
}
#endif