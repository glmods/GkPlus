#pragma once

#include <string>
#include <vector>

// The script queue's JSON, in the two shapes the queue actually asks for (see
// ScriptQueue.h for the format).
//
// **The codec is QuickJS**, not a hand-written parser - JS_ParseJSON and
// JS_JSONStringify, on a private JSRuntime owned by this file. Everything the
// interface offers is a thin call into those, so escaping, number formatting and
// the JSON grammar are the engine's problem rather than ours.
//
// The private runtime is what makes that possible: this runs on **both** game
// threads - the queue's producer and the four writer hooks are executor-side, the
// consumer is main-side - and a JSRuntime may only be used from one thread at a
// time, so borrowing the script host's context would race whatever a script was
// doing. Json.cpp keeps its own behind a lock, with no modules and no scripts in
// it, so nothing here is observable from a script or vice versa.
//
// Everything below is **UTF-8**, like any other JSON. The engine's ANSI strings
// are transcoded at the queue's edges by `gk::Encoding`; this file never sees a
// codepage.
//
// It is deliberately the smallest codec that answers the two questions the queue
// asks, and it builds no tree - a message that is not a string is handed on as
// text for JS_ParseJSON to read properly:
//
//   * is this text a JSON document at all - and which kind?
//   * if it is a string, what does it spell?
//
// Classify validates a document all the way down even though only the top-level
// kind is returned. That is not thoroughness for its own sake: `{a}.gcs` is a
// legal Windows file name, so "starts with a brace" cannot decide anything, and
// telling it from `{"a":1}` means walking to the end.
namespace gk::json {

// The seven top-level shapes of a JSON document. Invalid covers "not JSON",
// which on the queue means a bare .gcs name from a peer without GkPlus.
enum class Kind {
  Invalid,
  Null,
  Bool,
  Number,
  String,
  Array,
  Object,
};

// Validates `text` as exactly one complete JSON document and returns its
// top-level kind. For Kind::String, `value` (when given) receives the decoded
// characters.
//
// This is `JSON.parse` semantics exactly, because it *is* JSON.parse: strict
// grammar, and trailing content is rejected ("unexpected data at the end",
// quickjs.c:50003) - which is what keeps a file name like `{a}.gcs` from reading
// as an object on the two paths where a bare name can still arrive.
//
// The parsed value is discarded; only its kind and, for a string, its text
// survive. A message that is not a string is handed on as text and parsed
// properly by the script host's own context.
Kind Classify(const char *text, std::string *value = nullptr);

// `value` as a JSON string literal, quotes included - `JSON.stringify` of a
// string. This is how a .gcs name becomes a payload, after `Utf8FromGameText`,
// since `value` is expected to be UTF-8 already.
//
// Always returns a document: on the only failure QuickJS has here - an
// allocation - it degrades to `""` rather than to something no parser accepts,
// because ScriptQueuePayload's contract rests on that.
std::string Quote(const char *value);

// --- the payload envelope ----------------------------------------------------
//
// Every payload on either queue is one JSON object:
//
//     {"kind": "<kind>", "body": <anything>}
//
// This file owns the *shape*; the vocabulary of kinds is ScriptQueue.h's. That
// split is why these take a `kind` string rather than an enum - a kind this build
// does not know is still a well-formed envelope, and the consumer needs to say so
// rather than have the codec reject it.
//
// **The envelope is what removes the old ambiguity.** The format used to be "a
// JSON string is a file name, anything else is a message", which mis-read a .gcs
// literally called `123` as a message. The test is now "is this an object with a
// string `kind` and a `body`?", and the only file name that could collide would
// have to contain `"` and `:` - both illegal in a Windows path. Anything that is
// not an envelope is a bare name, with no residual doubt.

// Composes an envelope. `body_json` must be a complete JSON document; a null or
// unparseable one becomes `null`, so the result is a valid document either way -
// ScriptQueuePayload's contract rests on that, exactly as it does for Quote.
std::string Envelope(const char *kind, const char *body_json);

// Splits one apart. True only for an object carrying a string `kind` and a
// `body`; an array, a bare string, a number and anything unparseable are all
// false, which is what puts them on the residual bare-name path.
//
// `*body_json` receives the body **as JSON text**, not decoded - a message body
// is handed on for the script host's own context to parse, and a file or command
// body is a JSON string the caller decodes with Classify. One rule for all three.
bool OpenEnvelope(const char *text, std::string *kind, std::string *body_json);

// --- a document that can be read and written by path --------------------------
//
// The queue needs no tree; `src/Settings` does - it has to *update* one key of a
// file without disturbing the rest of it, because the rest of it belongs to
// somebody else's mod. That is the whole reason this exists rather than the
// settings store keeping its own struct and re-serialising it: a mod's section
// has to survive a build that has never heard of it.
//
// The tree is a `JSValue` in the same private runtime, and every operation takes
// the same lock, so the rules at the top of this file apply unchanged. The
// interface is deliberately text in and text out: one representation for every
// type, no variant to maintain here, and `Classify` already decodes the leaves.
//
// A path is dot-separated (`"core.render.msaa"`). A key containing a dot cannot
// be addressed - which is a real limit, and the reason the key vocabulary is
// ours rather than free-form.
class Document {
public:
  Document();
  ~Document();
  Document(const Document &) = delete;
  Document &operator=(const Document &) = delete;

  // Replaces the contents. False - leaving an empty object - unless `text` is one
  // complete JSON **object**: an array or a bare number is a document no path can
  // address, and silently keeping it would make every later Get fail obscurely.
  bool Parse(const char *text);
  // The whole document. `pretty` indents by two spaces, which is what a file a
  // human may edit wants.
  std::string Stringify(bool pretty) const;

  // The value at `path` as JSON text, or "" when any step of it is missing.
  std::string Get(const char *path) const;
  // The kind of the value at `path`, and Kind::Invalid when there is nothing
  // there - JSON has no undefined, so those are the same answer. Cheaper than
  // Get for a caller that only wants to know whether a subtree is an object,
  // since it neither stringifies the subtree nor parses it back.
  //
  // These two are the only operations for which an **empty path means the
  // document itself** (always Kind::Object, listing the top level). Get, Set and
  // Remove all refuse it, because there an accidental "" would read or replace
  // the whole file; asking what kind the root is, or what is in it, is safe.
  Kind KindAt(const char *path) const;
  // The enumerable keys of the object at `path`, in insertion order. Empty when
  // that is not an object - including when it is an array, whose indices are not
  // addressable by path anyway.
  std::vector<std::string> Keys(const char *path) const;
  // `json` must be one complete JSON document. Intermediate steps that do not
  // exist are created as objects, and one that exists but is not an object is
  // replaced by one - a path always wins over whatever was in its way.
  bool Set(const char *path, const char *json);
  // False when the leaf was not there to begin with.
  bool Remove(const char *path);

private:
  void *root_; // a JSValue, in the codec's runtime and freed under its lock
};

} // namespace gk::json
