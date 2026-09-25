// Small shared helpers: string formatting and UTF-8 conversion.
#pragma once

#include <string>

namespace core {

// printf-style formatting into a std::wstring.
std::wstring Fmt(const wchar_t* fmt, ...);

// UTF-16 to UTF-8.
std::string ToUtf8(const std::wstring& w);
// UTF-8 to UTF-16; invalid bytes become U+FFFD.
std::wstring FromUtf8(const std::string& s);

}  // namespace core
