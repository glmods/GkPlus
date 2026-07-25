#include "Tokens.h"

#include "Core.h"

namespace gk {
Tokens *GetTokensTable() {
  Tokens *t;
  GetObjectAtOffset(t, 0x007b6af8);
  return t;
}

void SetOrCreateToken(Tokens *tokens, const char *name, float value) {
  ThisCall<void, Tokens *, const char *, float> fn;
  GetObjectAtOffset(fn, 0x004d35f0);
  fn(tokens, name, value);
}

bool GetTokenValue(Tokens *tokens, float *value, const char *name) {
  ThisCall<bool, Tokens *, float *, const char *> fn;
  GetObjectAtOffset(fn, 0x004d3910);
  return fn(tokens, value, name);
}

void SetTokenValue(Tokens *tokens, const char *name, float value) {
  ThisCall<void, Tokens *, const char *, float> fn;
  GetObjectAtOffset(fn, 0x004d38a0);
  fn(tokens, name, value);
}

bool FindTokenWithValue(Tokens *tokens, float value, char **name) {
  ThisCall<bool, Tokens *, float, char **> fn;
  GetObjectAtOffset(fn, 0x004d3a60);
  return fn(tokens, value, name);
}
} // namespace gk
