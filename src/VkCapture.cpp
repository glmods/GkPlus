#include "VkCapture.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

#include "Core.h"
#include <renderdoc_app.h>

namespace gk {
namespace vulkan {
namespace {

RENDERDOC_API_1_4_1 *Api = nullptr;
bool Attempted = false;
bool Armed = false;
bool Capturing = false;
uint32_t Captures = 0;
std::string Status = "not loaded";
std::string CapturePath = "(renderdoc's own)";

// The 32-bit build, which is not the one beside the UI. RenderDoc installs x64
// at the root and x86 under `x86\`, and loading the wrong one fails with a bare
// "not a valid Win32 application" that says nothing about bitness.
const char *kDefaultPath = "C:\\Program Files\\RenderDoc\\x86\\renderdoc.dll";

// `<gl.exe's directory>\gkplus_frame`, absolute. A *relative* template is
// resolved by RenderDoc when it writes the file, against the process's current
// directory - and Gunlok moves that constantly (`SetCurrentDirectoryToGLDir`
// per asset category), so a capture taken during a level load lands in
// whichever GLDir the loader was last in. The capture is produced either way;
// it is just not where anyone looks for it.
std::string DefaultCaptureTemplate() {
  char module[MAX_PATH] = {};
  DWORD n = ::GetModuleFileNameA(nullptr, module, sizeof(module));
  if (n == 0 || n >= sizeof(module)) {
    return "gkplus_frame";
  }
  std::string path(module, n);
  const size_t slash = path.find_last_of("\\/");
  if (slash == std::string::npos) {
    return "gkplus_frame";
  }
  return path.substr(0, slash + 1) + "gkplus_frame";
}

} // namespace

void LoadRenderDoc() {
  if (Attempted) {
    return;
  }
  Attempted = true;

  char enabled[8] = {};
  if (::GetEnvironmentVariableA("GKPLUS_RENDERDOC", enabled, sizeof(enabled)) ==
          0 ||
      enabled[0] == '0') {
    Status = "off (GKPLUS_RENDERDOC unset)";
    return;
  }

  // An already-loaded module wins: if the game was launched from RenderDoc's
  // UI, its DLL is present and injecting a second copy from a different path
  // would be a second capture layer.
  HMODULE module = ::GetModuleHandleA("renderdoc.dll");
  const bool renderdoc_launched_us = module != nullptr;
  if (module == nullptr) {
    char path[MAX_PATH] = {};
    if (::GetEnvironmentVariableA("GKPLUS_RENDERDOC_DLL", path, sizeof(path)) ==
        0) {
      std::snprintf(path, sizeof(path), "%s", kDefaultPath);
    }
    module = ::LoadLibraryA(path);
    if (module == nullptr) {
      Status = std::string("could not load ") + path;
      DebugWrite("gkplus: renderdoc: " + Status + "\n");
      return;
    }
  }

  auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(
      ::GetProcAddress(module, "RENDERDOC_GetAPI"));
  if (get_api == nullptr || get_api(eRENDERDOC_API_Version_1_4_1,
                                    reinterpret_cast<void **>(&Api)) != 1) {
    Api = nullptr;
    Status = "renderdoc.dll has no usable 1.4.1 API";
    DebugWrite("gkplus: renderdoc: " + Status + "\n");
    return;
  }

  // Captures land beside the game rather than in RenderDoc's default temp
  // location, so a run leaves its evidence where the rest of the session's
  // artifacts are - but ONLY when we loaded renderdoc.dll ourselves. If the
  // game was launched from RenderDoc's UI, the UI picked the template and
  // knows where to look for the result; overriding it there moves the file out
  // from under the thing that asked for it.
  if (!renderdoc_launched_us) {
    CapturePath = DefaultCaptureTemplate();
    Api->SetCaptureFilePathTemplate(CapturePath.c_str());
  }
  // The overlay is RenderDoc's own text drawn into the swapchain; it would sit
  // on top of the renderer being debugged and, more to the point, it is drawn
  // by hooking present in a way that has its own failure modes. The REPL says
  // whether a capture happened.
  Api->MaskOverlayBits(eRENDERDOC_Overlay_None, eRENDERDOC_Overlay_None);
  Status = renderdoc_launched_us ? "loaded (renderdoc launched us)" : "loaded";
  DebugWrite("gkplus: renderdoc: loaded, captures go to " + CapturePath +
             "_NNNN.rdc\n");
}

bool RenderDocLoaded() { return Api != nullptr; }

bool TriggerCapture() {
  if (Api == nullptr) {
    return false;
  }
  Armed = true;
  return true;
}

void BeginFrameCaptureIfArmed() {
  if (Api == nullptr || !Armed || Capturing) {
    return;
  }
  // Null device and window: "whatever is active". With both a D3D9 and a Vulkan
  // device live in this process that is ambiguous in general - but the call is
  // made from inside our own Vulkan frame, between vkBeginCommandBuffer and
  // present, which is where RenderDoc resolves it to the Vulkan one.
  Api->StartFrameCapture(nullptr, nullptr);
  Capturing = true;
}

void EndFrameCaptureIfArmed() {
  if (Api == nullptr || !Capturing) {
    return;
  }
  Api->EndFrameCapture(nullptr, nullptr);
  Capturing = false;
  Armed = false;
  ++Captures;
}

std::string FormatCaptureStatus() {
  char line[256];
  std::snprintf(line, sizeof(line),
                "renderdoc: %s   armed: %s   captures: %u\n", Status.c_str(),
                Armed ? "yes" : "no", Captures);
  // The path, because "no capture was produced" and "a capture was produced
  // somewhere else" look identical from the outside. `armed: yes` on a later
  // poll is the other half: the frame that would end the capture never ran.
  return std::string(line) + "path: " + CapturePath + "_NNNN.rdc\n";
}

} // namespace vulkan
} // namespace gk
