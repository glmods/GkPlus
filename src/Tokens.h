#pragma once

#include "List.h"
#include "Memory.h"

#include <cstddef>

namespace gk {
// The 8-byte payload of a token list node, pool_alloc'd by CreateToken
// @ 0x004d35f0 and pool-freed by FreeTokens @ 0x004d3800.
struct TokenData {
  pool_string name;
  float value;
};
static_assert(sizeof(TokenData) == 8);

// The global token table @ 0x007b6af8 is a plain List - what was previously
// modelled as a two-field {head, count} struct is its first half, with the
// entry_pointers cache CreateToken frees at +0x08 making up the rest.
using Token = List_Member<pool_unique_ptr<TokenData>>;
using Tokens = List<pool_unique_ptr<TokenData>>;
static_assert(sizeof(Token) == 0x10);
static_assert(sizeof(Tokens) == 0x10);

// --- Native API --------------------------------------------------------------

// The global token list @ 0x007b6af8. Iterate with begin()/end().
Tokens *GetTokensTable();

// CreateToken @ 0x004d35f0 appends a token (name/value) to the list.
void CreateToken(Tokens *tokens, const char *name, float value);
} // namespace gk
