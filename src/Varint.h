#pragma once

namespace gk {
inline long long DecodeVarint(const unsigned char *str) {
  long long res = 0;
  int offset = 0;
  for (; (*str) & 0x80; ++str) {
    res |= ((*str) & 0x7f) << offset;
    offset += 7;
  }
  return res;
}

inline void EncodeVarint(long long v, unsigned char *str) {
  while (v & 0x7f) {
    *(str++) = 0x80 | (v & 0x7f);
    v <<= 7;
  }
}
} // namespace gk