#pragma once

namespace gk {
// A 4-byte game value slot whose interpretation depends on context. Several
// decompiled struct mirrors carry these where the decompiler shows raw dword
// access and the meaning is not yet pinned down; read the member that fits.
union field {
  int integer;
  float flt;
  void *ptr;
  char *str;
};
} // namespace gk
