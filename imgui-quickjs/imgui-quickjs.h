#pragma once

#include <quickjs.h>

#ifdef __cplusplus
extern "C" {
#endif

JSModuleDef *js_init_module_imgui(JSContext *ctx);

#ifdef __cplusplus
}
#endif