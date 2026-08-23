#include "VkCapture.h"

namespace gk {
namespace vulkan {
void LoadRenderDoc() {}
bool RenderDocLoaded() { return false; }
bool TriggerCapture() { return false; }
void BeginFrameCaptureIfArmed() {}
void EndFrameCaptureIfArmed() {}
void CaptureStagingBatch(uint32_t) {}
void BeginBatchCaptureIfArmed(uint32_t, void *) {}
void EndBatchCaptureIfArmed(uint32_t) {}
std::string FormatCaptureStatus() { return ""; }
} // namespace vulkan
} // namespace gk
