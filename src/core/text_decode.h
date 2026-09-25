// Reads a file and decodes its bytes to text, detecting the encoding automatically.
#pragma once

#include <windows.h>

#include <string>

namespace core {

enum class Encoding { Utf8, Utf8Bom, Utf16LE, Utf16BE, Ansi };

struct DecodedText {
    std::wstring text;
    Encoding     encoding = Encoding::Utf8;
    UINT         codepage = CP_UTF8;   // the ANSI code page actually used, for Ansi
};

// Reads a whole file, shared with writers. False with a reason in `error` on failure.
bool ReadFileBytes(const std::wstring& path, std::string* bytes, std::wstring* error);

// Detects the encoding and decodes; never fails. Order: BOM, BOM-less UTF-16,
// valid UTF-8, then the system ANSI code page.
void DecodeText(const std::string& bytes, DecodedText* out);

// "UTF-8", "UTF-8 (BOM)", "UTF-16 LE", "Windows-1254"...
std::wstring EncodingName(Encoding e, UINT codepage);

}  // namespace core
