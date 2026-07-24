#include "Tokens.h"

#include "Core.h"

namespace gk {
Tokens *GetTokensTable() {
  Tokens *t;
  GetObjectAtOffset(t, 0x007b6af8);
  return t;
}

void CreateToken(Tokens *tokens, const char *name, float value) {
  ThisCall<void, Tokens *, const char *, float> fn;
  GetObjectAtOffset(fn, 0x004d35f0);
  fn(tokens, name, value);
}
} // namespace gk
