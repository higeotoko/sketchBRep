#include "sbr/TextEncoding.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <iconv.h>
#endif

namespace sbr {

std::string toShiftJis(const std::string& utf8) {
  bool ascii = true;
  for (unsigned char c : utf8) ascii = ascii && c < 0x80;
  if (ascii) return utf8;
#ifdef _WIN32
  const int wide = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (wide <= 0) return std::string(utf8.size(), '?');
  std::wstring w(static_cast<std::size_t>(wide), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), wide);
  const int bytes = WideCharToMultiByte(932, 0, w.data(), wide, nullptr, 0, "?", nullptr);
  if (bytes <= 0) return std::string(utf8.size(), '?');
  std::string out(static_cast<std::size_t>(bytes), '\0');
  WideCharToMultiByte(932, 0, w.data(), wide, out.data(), bytes, "?", nullptr);
  return out;
#else
  iconv_t cd = iconv_open("CP932", "UTF-8");
  if (cd == reinterpret_cast<iconv_t>(-1)) return utf8;
  std::string out;
  char buffer[256];
  char* in = const_cast<char*>(utf8.data());
  std::size_t left = utf8.size();
  while (left > 0) {
    char* o = buffer;
    std::size_t room = sizeof buffer;
    const std::size_t r = iconv(cd, &in, &left, &o, &room);
    out.append(buffer, static_cast<std::size_t>(o - buffer));
    if (r == static_cast<std::size_t>(-1)) {
      if (errno == E2BIG) continue;
      // No code for it (or broken UTF-8): '?' for the whole character.
      out.push_back('?');
      std::size_t skip = 1;
      while (skip < left && (static_cast<unsigned char>(in[skip]) & 0xC0) == 0x80) ++skip;
      in += skip;
      left -= skip;
    }
  }
  iconv_close(cd);
  return out;
#endif
}

}  // namespace sbr
