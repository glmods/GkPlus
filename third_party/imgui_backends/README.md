# Vendored ImGui Vulkan backend

`imgui_impl_vulkan.cpp`, verbatim from **Dear ImGui v1.92.8** — the same version vcpkg
installs, whose `imgui_impl_vulkan.h` is used from the vcpkg include directory.

## Why it is here

vcpkg builds the `vulkan-binding` feature **without** `IMGUI_IMPL_VULKAN_NO_PROTOTYPES`, so
its prebuilt object calls `vkCreateFence` and friends directly and needs a Vulkan import
library. GkPlus deliberately has none: it reaches Vulkan through volk's runtime loading so
that `d3d8.dll` keeps loading — and the game keeps starting — on a machine with no Vulkan at
all. Linking `vulkan-1.lib` would make Vulkan a hard load-time dependency of a **d3d8 proxy
for a 2000 game**, which is the one thing worth avoiding here.

Delay-loading it was the other candidate and is not available: the installed Vulkan SDK has
no `Lib32`, so there is no 32-bit import library to delay-load, and gl.exe is x86.

Compiling this file into GkPlus with `IMGUI_IMPL_VULKAN_NO_PROTOTYPES` makes the backend go
through function pointers, which `ImGui_ImplVulkan_LoadFunctions` fills from volk in
`src/VkRenderer.cpp`.

Only the `.cpp` is vendored; the header still comes from vcpkg. If an imgui bump changes the
header, this file stops compiling — which is the intended failure mode, and the signal to
refresh it.

## Refreshing

Copy `backends/imgui_impl_vulkan.cpp` out of the imgui source for the version vcpkg pins:

    vcpkg/buildtrees/imgui/src/<version>/backends/imgui_impl_vulkan.cpp

The linker takes this object rather than the one inside `imgui.lib` because object files are
resolved before library members. If that ever stops being true it shows up as a duplicate
symbol error at link time, not as silently wrong behaviour.
