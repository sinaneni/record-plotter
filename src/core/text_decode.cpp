// File reading and text encoding detection.
#include "core/text_decode.h"

#include <algorithm>

#include "core/util.h"

namespace core {
namespace {

// The system locale's ANSI code page (GetACP() reports UTF-8 for this process).
UINT SystemAnsiCodePage() {
    UINT cp = 0;
    if (GetLocaleInfoEx(LOCALE_NAME_SYSTEM_DEFAULT,
                        LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
                        reinterpret_cast<LPWSTR>(&cp), sizeof(cp) / sizeof(wchar_t)) == 0 ||
        cp == 0 || cp == CP_UTF8) {
        cp = 1252;
    }
    return cp;
}

void Widen(const char* p, size_t n, UINT cp, DWORD flags, std::wstring* out) {
    out->clear();
    if (n == 0) return;
    const int len = MultiByteToWideChar(cp, flags, p, static_cast<int>(n), nullptr, 0);
    if (len <= 0) return;
    out->resize(static_cast<size_t>(len));
    MultiByteToWideChar(cp, flags, p, static_cast<int>(n), &(*out)[0], len);
}

void FromUtf16(const char* p, size_t n, bool big_endian, std::wstring* out) {
    const size_t count = n / 2;
    out->resize(count);
    for (size_t i = 0; i < count; ++i) {
        const unsigned char a = static_cast<unsigned char>(p[2 * i]);
        const unsigned char b = static_cast<unsigned char>(p[2 * i + 1]);
        (*out)[i] = static_cast<wchar_t>(big_endian ? (a << 8) | b : (b << 8) | a);
    }
}

bool IsValidUtf8(const char* p, size_t n) {
    // Fails on the first ill-formed sequence; counts only.
    if (n == 0) return true;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, static_cast<int>(n), nullptr,
                               0) > 0;
}

}  // namespace

bool ReadFileBytes(const std::wstring& path, std::string* bytes, std::wstring* error) {
    bytes->clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (error) {
            *error = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
                         ? L"File not found."
                         : Fmt(L"The file could not be opened (Windows error %lu).", e);
        }
        return false;
    }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(f, &size)) {
        CloseHandle(f);
        if (error) *error = L"The file size could not be read.";
        return false;
    }
    // Size limit, so a huge file gives an error instead of running out of memory.
    constexpr LONGLONG kMaxBytes = 2LL * 1024 * 1024 * 1024 - 1;
    if (size.QuadPart > kMaxBytes) {
        CloseHandle(f);
        if (error) *error = L"The file is larger than 2 GB, which this version cannot open.";
        return false;
    }
    bytes->resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < bytes->size()) {
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>((std::min)(bytes->size() - done,
                                                         static_cast<size_t>(1 << 26)));
        if (!ReadFile(f, &(*bytes)[done], want, &got, nullptr)) {
            CloseHandle(f);
            if (error) *error = L"The file could not be read.";
            return false;
        }
        if (got == 0) break;   // the file shrank under us: keep what was read
        done += got;
    }
    bytes->resize(done);
    CloseHandle(f);
    return true;
}

void DecodeText(const std::string& bytes, DecodedText* out) {
    const char* p = bytes.data();
    const size_t n = bytes.size();
    const auto u = [p](size_t i) { return static_cast<unsigned char>(p[i]); };

    // 1. A BOM.
    if (n >= 3 && u(0) == 0xEF && u(1) == 0xBB && u(2) == 0xBF) {
        out->encoding = Encoding::Utf8Bom;
        out->codepage = CP_UTF8;
        Widen(p + 3, n - 3, CP_UTF8, 0, &out->text);
        return;
    }
    if (n >= 2 && u(0) == 0xFF && u(1) == 0xFE) {
        out->encoding = Encoding::Utf16LE;
        out->codepage = 1200;
        FromUtf16(p + 2, n - 2, false, &out->text);
        return;
    }
    if (n >= 2 && u(0) == 0xFE && u(1) == 0xFF) {
        out->encoding = Encoding::Utf16BE;
        out->codepage = 1201;
        FromUtf16(p + 2, n - 2, true, &out->text);
        return;
    }

    // 2. UTF-16 without a BOM: zero bytes in nearly every other position.
    const size_t probe = (std::min)(n & ~static_cast<size_t>(1), static_cast<size_t>(8192));
    if (probe >= 16) {
        size_t zero_even = 0, zero_odd = 0;
        for (size_t i = 0; i < probe; i += 2) {
            if (u(i) == 0) ++zero_even;
            if (u(i + 1) == 0) ++zero_odd;
        }
        const size_t half = probe / 2;
        if (zero_odd > half * 7 / 10 && zero_even < half / 10) {
            out->encoding = Encoding::Utf16LE;
            out->codepage = 1200;
            FromUtf16(p, n, false, &out->text);
            return;
        }
        if (zero_even > half * 7 / 10 && zero_odd < half / 10) {
            out->encoding = Encoding::Utf16BE;
            out->codepage = 1201;
            FromUtf16(p, n, true, &out->text);
            return;
        }
    }

    // 3. Valid UTF-8 (which includes pure ASCII).
    if (IsValidUtf8(p, n)) {
        out->encoding = Encoding::Utf8;
        out->codepage = CP_UTF8;
        Widen(p, n, CP_UTF8, 0, &out->text);
        return;
    }

    // 4. The system ANSI code page; on 1252, any Turkish-only byte switches to 1254.
    out->encoding = Encoding::Ansi;
    out->codepage = SystemAnsiCodePage();
    if (out->codepage == 1252) {
        for (size_t i = 0; i < n; ++i) {
            const unsigned char c = u(i);
            if (c == 0xD0 || c == 0xDD || c == 0xDE || c == 0xF0 || c == 0xFD || c == 0xFE) {
                out->codepage = 1254;
                break;
            }
        }
    }
    Widen(p, n, out->codepage, 0, &out->text);
}

std::wstring EncodingName(Encoding e, UINT codepage) {
    switch (e) {
        case Encoding::Utf8:    return L"UTF-8";
        case Encoding::Utf8Bom: return L"UTF-8 (BOM)";
        case Encoding::Utf16LE: return L"UTF-16 LE";
        case Encoding::Utf16BE: return L"UTF-16 BE";
        case Encoding::Ansi:    break;
    }
    return Fmt(L"Windows-%u", codepage);
}

}  // namespace core
