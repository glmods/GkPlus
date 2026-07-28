#pragma once

#include <string>

namespace gk {
// The one place the game's text encoding meets JSON's.
//
// **Everything the engine holds in a `char *` is ANSI** - bytes in the active
// codepage (CP_ACP). A `.gls` is read from disk as bytes and its tokens are
// strdup'd verbatim; the console builds its line from DIK scan codes, so it is
// ASCII by construction; and `fopen` interprets the `char *` it is handed through
// that same codepage. **JSON is UTF-8** - the format requires it, and both
// decoders that read our payloads assume it: `JS_ParseJSON` for a message and
// `JS_ToCString` on the way back out.
//
// So the script queue transcodes at its edges rather than carrying whichever
// bytes it was given. The alternative - passing high bytes through untouched -
// looks like it works, because `gk::json`'s own string decoder is byte-exact, but
// it breaks the moment a name is embedded in a *message*: QuickJS decodes that
// document, replaces every invalid sequence with U+FFFD, and the script receives
// a path that opens nothing.
//
// In practice almost everything here is ASCII, where both functions are the
// identity. What they are for is the one case that is not: a `.gls` authored on a
// localized Windows naming a script file in that language.

// ANSI (CP_ACP) -> UTF-8, for a string on its way into a payload.
std::string Utf8FromGameText(const char *text);

// UTF-8 -> ANSI (CP_ACP), for a string on its way back to the engine - to `fopen`,
// or to any other `char *` the game will read.
//
// Round-tripping is lossless for anything that came *from* CP_ACP, which is the
// case that matters. A codepoint the active codepage cannot represent is a
// genuine loss, but it was equally unopenable before: `fopen` speaks the same
// codepage.
std::string GameTextFromUtf8(const char *text);
} // namespace gk
