#include "Chunks.h"
#include "Core.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>

namespace gk {

using ChunkBuilder = StdCall<void *, void *, char *>;
static FastCall<void, const char *, const char *, ChunkBuilder *> Register;

static void __fastcall HookedRegister(const char *idChunk, const char *idParent,
                                      ChunkBuilder *builder) {
  DebugWrite("Registering chunk {}", idChunk);
  if (idParent) {
    DebugWrite(" with parent {}", idParent);
  }
  DebugWrite("\n");

  Register(idChunk, idParent, builder);
}

ChunksModule::ChunksModule(lua_State *L) : Module{L} {
  GetObjectAtOffset(Register, 0x005d4ae0);
  DetourAttach(&Register, HookedRegister);
}
ChunksModule::~ChunksModule() { DetourDetach(&Register, HookedRegister); }
} // namespace gk
