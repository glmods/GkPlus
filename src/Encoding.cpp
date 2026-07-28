#include "Encoding.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vector>

namespace gk {
namespace {

// Both directions are the same two calls with the codepages swapped, so they
// share one body. A failure returns the input unchanged rather than an empty or
// half-converted string: that degrades to the behaviour this replaced - the exact
// bytes travel - instead of silently naming a different file.
std::string Convert(const char *text, unsigned from, unsigned to) {
  if (!text || !*text) {
    return {};
  }
  // MB_ERR_INVALID_CHARS so a byte sequence that is not valid in `from` fails
  // loudly here rather than becoming U+FFFD several steps later. It is only legal
  // for the codepages used below.
  int wide_len =
      MultiByteToWideChar(from, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
  if (wide_len <= 0) {
    return text;
  }
  std::vector<wchar_t> wide(static_cast<size_t>(wide_len));
  if (MultiByteToWideChar(from, MB_ERR_INVALID_CHARS, text, -1, wide.data(),
                          wide_len) <= 0) {
    return text;
  }

  int out_len =
      WideCharToMultiByte(to, 0, wide.data(), -1, nullptr, 0, nullptr, nullptr);
  if (out_len <= 0) {
    return text;
  }
  std::string out(static_cast<size_t>(out_len), '\0');
  if (WideCharToMultiByte(to, 0, wide.data(), -1, out.data(), out_len, nullptr,
                          nullptr) <= 0) {
    return text;
  }
  out.resize(static_cast<size_t>(out_len) - 1); // drop the NUL the API counts
  return out;
}

} // namespace

std::string Utf8FromGameText(const char *text) {
  return Convert(text, CP_ACP, CP_UTF8);
}

std::string GameTextFromUtf8(const char *text) {
  return Convert(text, CP_UTF8, CP_ACP);
}
} // namespace gk
