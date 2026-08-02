#include "VkCapture.h"

namespace gk {
namespace vulkan {
void LoadRenderDoc() {}
bool RenderDocLoaded() { return false; }
bool TriggerCapture() { return false; }
void BeginFrameCaptureIfArmed() {}
void EndFrameCaptureIfArmed() {}
std::string FormatCaptureStatus() { return ""; }
} // namespace vulkan
} // namespace gk
