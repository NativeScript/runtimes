#pragma once
// UTF-16 <-> UTF-8 for engine backends whose native string API is UTF-8 only (QuickJS, Hermes).
// V8 and JSC take and return UTF-16 code units directly, so they never come through here.
#include <cstdint>
#include <string>

namespace nativescript::engine::utf16 {

inline std::string toUtf8(const char16_t* data, size_t length) {
  std::string out;
  out.reserve(length);
  for (size_t i = 0; i < length; i++) {
    uint32_t cp = data[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < length && data[i + 1] >= 0xDC00 &&
        data[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (data[i + 1] - 0xDC00);
      i++;
    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
      cp = 0xFFFD;  // unpaired surrogate
    }
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

inline std::u16string fromUtf8(const std::string& in) {
  std::u16string out;
  out.reserve(in.size());
  size_t i = 0, n = in.size();
  while (i < n) {
    unsigned char c = static_cast<unsigned char>(in[i]);
    uint32_t cp;
    size_t extra;
    if (c < 0x80) { cp = c; extra = 0; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { cp = 0xFFFD; extra = 0; }
    if (i + extra >= n + (extra ? 0 : 1) && extra) { cp = 0xFFFD; extra = 0; }
    for (size_t k = 1; k <= extra; k++) {
      unsigned char cc = static_cast<unsigned char>(in[i + k]);
      if ((cc & 0xC0) != 0x80) { cp = 0xFFFD; extra = k - 1; break; }
      cp = (cp << 6) | (cc & 0x3F);
    }
    i += extra + 1;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<char16_t>(cp));
    }
  }
  return out;
}

}  // namespace nativescript::engine::utf16
