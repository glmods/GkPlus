#include "Tokens.h"

#include "JsBindings.h"

#include <iterator>
#include <string>
#include <vector>

namespace gk::js {
namespace {

JSClassID TokensClassId;

// Unlike actors and roles, a token has no id and no object identity - it is an
// 8-byte {name, float} pair - so the collection is keyed by name and resolves to
// the bare value rather than to a wrapper. Object.entries(tokens) is the
// name/value view.
JSValue LookupTokenByName(JSContext *ctx, const char *name) {
  Tokens *tokens = GetTokensTable();
  float value = 0.0f;
  if (!tokens || !GetTokenValue(tokens, &value, name)) {
    return JS_UNDEFINED;
  }
  return JS_NewFloat64(ctx, value);
}

void CollectTokenKeys(std::vector<std::string> *out) {
  Tokens *tokens = GetTokensTable();
  if (!tokens) {
    return;
  }
  out->reserve(tokens->size());
  // begin()/end() terminate on the sentinel, which is a bare List_Member_Base
  // with no `data` - walking on `cur->next != sentinel` would over-read the heap
  // (see List.h).
  for (const pool_unique_ptr<TokenData> &token : *tokens) {
    if (token && token->name) {
      out->emplace_back(token->name.get());
    }
  }
}

unsigned CountTokens() {
  Tokens *tokens = GetTokensTable();
  return tokens ? static_cast<unsigned>(tokens->size()) : 0;
}

// `tokens["score"] = 0`. SetOrCreateToken is an upsert - it overwrites in place
// when the name already exists and allocates only when it does not - which is
// exactly assignment semantics, so there is no separate create() to call.
// (SetTokenValue would be wrong here: it silently does nothing for a token that
// does not exist yet.) It also strdups the name, so our buffer is not retained.
int AssignToken(JSContext *ctx, const char *name, JSValueConst value) {
  Tokens *tokens = GetTokensTable();
  if (!tokens) {
    JS_ThrowInternalError(ctx, "the token table is not available");
    return -1;
  }
  double v = 0.0;
  if (JS_ToFloat64(ctx, &v, value)) {
    return -1;
  }
  SetOrCreateToken(tokens, name, static_cast<float>(v));
  return 1;
}

// No lookup_id: tokens have no integer keys, so a wholly-decimal key falls
// through to the name lookup and a token literally called "5" still resolves.
//
// Two engine behaviours come through the indexer unchanged (see Tokens.h):
// lookup is case-insensitive, and `tokens["rand(6)"]` is a pseudo-token that
// skips the list entirely and yields a uniform integer in [0, 6). That last one
// means `"rand(6)" in tokens` is true even though it is not in Object.keys -
// legal for an exotic object, and the same thing a Proxy can do.
const CollectionOps TokensOps = {
    .class_name = "Tokens",
    .lookup_id = nullptr,
    .lookup_name = LookupTokenByName,
    .collect_keys = CollectTokenKeys,
    .count = CountTokens,
    .assign = AssignToken,
    .props = nullptr,
    .props_len = 0,
};

} // namespace

JSValue NewTokensNamespace(JSContext *ctx) {
  return NewCollection(ctx, &TokensClassId, &TokensOps);
}

} // namespace gk::js
