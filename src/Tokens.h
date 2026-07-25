#pragma once

#include "List.h"
#include "Memory.h"

#include <cstddef>

namespace gk {
// The 8-byte payload of a token list node, pool_alloc'd by SetOrCreateToken
// @ 0x004d35f0 and pool-freed by FreeTokens @ 0x004d3800.
struct TokenData {
  pool_string name;
  float value;
};
static_assert(sizeof(TokenData) == 8);

// The global token table @ 0x007b6af8 is a plain List - what was previously
// modelled as a two-field {head, count} struct is its first half, with the
// entry_pointers cache SetOrCreateToken frees at +0x08 making up the rest.
//
// CAUTION: the engine's token functions read an RWLock at `this + 0x10`, i.e.
// immediately past this struct (0x007b6b08), and lock it for the whole call.
// The global is really a {List, RWLock} pair and only the List half is modelled
// here, so these functions may only ever be called with GetTokensTable() - hand
// one a locally-constructed Tokens and it locks whatever follows it in memory.
using Token = List_Member<pool_unique_ptr<TokenData>>;
using Tokens = List<pool_unique_ptr<TokenData>>;
static_assert(sizeof(Token) == 0x10);
static_assert(sizeof(Tokens) == 0x10);

// --- Native API --------------------------------------------------------------

// The global token list @ 0x007b6af8. Iterate with begin()/end().
Tokens *GetTokensTable();

// SetOrCreateToken @ 0x004d35f0 - an UPSERT, despite the `CreateToken` name it
// carried until it was read properly: it walks the list case-insensitively
// first and overwrites the value in place if the name is already there, and only
// allocates a node when it is not. Repeat calls never duplicate a token.
void SetOrCreateToken(Tokens *tokens, const char *name, float value);

// GetTokenValue @ 0x004d3910 - looks a token up by name, writing its value
// through `value`. False if there is no such token.
//
// Two behaviours worth knowing: the name compare is `_mbsicmp`, so lookup is
// CASE-INSENSITIVE; and a name of the form `rand(N)` is a pseudo-token that
// never touches the list at all - it draws from the calling thread's PRNG and
// yields a uniform integer in [0, N), always returning true.
bool GetTokenValue(Tokens *tokens, float *value, const char *name);

// SetTokenValue @ 0x004d38a0 - update-only, and SILENT if the token does not
// exist: it walks the list and returns having done nothing. Prefer
// SetOrCreateToken unless "only if it already exists" is what you mean.
void SetTokenValue(Tokens *tokens, const char *name, float value);

// FindTokenWithValue @ 0x004d3a60 - the reverse lookup, returning the first
// token whose value equals `value` (borrowed pointer into the token's own
// pool_string). False if there is none.
//
// Together these two are how the engine names actors: a token's value IS an
// actor id, rounded to an int. ConsoleParseActorName @ 0x004d6d90 does
// GetTokenValue -> ROUND -> GetActorById, and CommandGetActorName @ 0x00446d30
// inverts it with FindTokenWithValue((float)actor->id).
bool FindTokenWithValue(Tokens *tokens, float value, char **name);
} // namespace gk
