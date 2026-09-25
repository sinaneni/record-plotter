// Formatting and UTF-8 conversion helpers.
#include "util.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <vector>

namespace core {

std::wstring Fmt(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    // Measure first, then format into an exact-size buffer.
    const int n = _vscwprintf(fmt, ap);
    va_end(ap);
    if (n <= 0) return std::wstring();

    std::wstring out(static_cast<size_t>(n), L'\0');
    va_start(ap, fmt);
    vswprintf_s(&out[0], static_cast<size_t>(n) + 1, fmt, ap);
    va_end(ap);
    return out;
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    // Lenient: invalid bytes are replaced rather than failing.
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n);
    return out;
}

}  // namespace core
