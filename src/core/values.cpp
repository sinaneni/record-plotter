#include "core/values.h"

#include <charconv>
#include <cmath>

#include "core/util.h"

namespace core {
namespace {

bool IsDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }

bool IsSpace(wchar_t c) {
    // Includes the no-break spaces used as thousands separators.
    return c == L' ' || c == L'\t' || c == 0x00A0 || c == 0x202F;
}

bool IEquals(Cell a, const wchar_t* b) {
    size_t i = 0;
    for (; b[i]; ++i) {
        if (i >= a.size()) return false;
        wchar_t x = a[i], y = b[i];
        if (x >= L'A' && x <= L'Z') x = static_cast<wchar_t>(x - L'A' + L'a');
        if (y >= L'A' && y <= L'Z') y = static_cast<wchar_t>(y - L'A' + L'a');
        if (x != y) return false;
    }
    return i == a.size();
}

// Reads 1..max_digits digits at `*i`. False, with *i untouched, if there are none.
bool ReadInt(Cell c, size_t* i, int max_digits, int* value, int* digits = nullptr) {
    size_t j = *i;
    int v = 0, n = 0;
    while (j < c.size() && IsDigit(c[j]) && n < max_digits) {
        v = v * 10 + (c[j] - L'0');
        ++j;
        ++n;
    }
    if (n == 0) return false;
    // Reject runs longer than max_digits.
    if (j < c.size() && IsDigit(c[j])) return false;
    *i = j;
    *value = v;
    if (digits) *digits = n;
    return true;
}

void SkipSpaces(Cell c, size_t* i) {
    while (*i < c.size() && IsSpace(c[*i])) ++*i;
}

// Reads a time of day, with optional seconds, fraction and AM/PM, as seconds since midnight.
bool ReadTime(Cell c, size_t* i, double* secs) {
    size_t j = *i;
    int h = 0, m = 0, s = 0;
    if (!ReadInt(c, &j, 2, &h)) return false;
    if (j >= c.size() || c[j] != L':') return false;
    ++j;
    int md = 0;
    if (!ReadInt(c, &j, 2, &m, &md) || md != 2) return false;
    double frac = 0.0;
    if (j < c.size() && c[j] == L':') {
        ++j;
        int sd = 0;
        if (!ReadInt(c, &j, 2, &s, &sd) || sd != 2) return false;
        if (j + 1 < c.size() && (c[j] == L'.' || c[j] == L',') && IsDigit(c[j + 1])) {
            ++j;
            double scale = 0.1;
            while (j < c.size() && IsDigit(c[j])) {
                frac += (c[j] - L'0') * scale;
                scale *= 0.1;
                ++j;
            }
        }
    }
    // AM/PM, separated by a space or not.
    size_t k = j;
    SkipSpaces(c, &k);
    if (k + 1 < c.size() && (c[k + 1] == L'M' || c[k + 1] == L'm')) {
        const wchar_t a = c[k];
        if (a == L'A' || a == L'a' || a == L'P' || a == L'p') {
            if (h < 1 || h > 12) return false;
            const bool pm = (a == L'P' || a == L'p');
            if (h == 12) h = 0;
            if (pm) h += 12;
            j = k + 2;
        }
    }
    if (h > 24 || m > 59 || s > 60) return false;
    *secs = h * 3600.0 + m * 60.0 + s + frac;
    *i = j;
    return true;
}

// Skips a "Z" or UTC offset suffix.
void SkipZone(Cell c, size_t* i) {
    size_t j = *i;
    SkipSpaces(c, &j);
    if (j < c.size() && (c[j] == L'Z' || c[j] == L'z')) {
        *i = j + 1;
        return;
    }
    if (j < c.size() && (c[j] == L'+' || c[j] == L'-')) {
        size_t k = j + 1;
        int hh = 0, mm = 0, nd = 0;
        if (!ReadInt(c, &k, 4, &hh, &nd)) return;
        if (nd == 2 && k < c.size() && c[k] == L':') {
            ++k;
            if (!ReadInt(c, &k, 2, &mm)) return;
        } else if (nd != 2 && nd != 4) {
            return;
        }
        *i = k;
    }
}

}  // namespace

Cell Trim(Cell c) {
    size_t b = 0, e = c.size();
    while (b < e && IsSpace(c[b])) ++b;
    while (e > b && (IsSpace(c[e - 1]) || c[e - 1] == L'\r')) --e;
    if (e - b >= 2 && c[b] == L'"' && c[e - 1] == L'"') {
        ++b;
        --e;
        while (b < e && IsSpace(c[b])) ++b;
        while (e > b && IsSpace(c[e - 1])) --e;
    }
    return c.substr(b, e - b);
}

bool IsMissing(Cell c) {
    if (c.empty()) return true;
    return IEquals(c, L"-") || IEquals(c, L"--") || IEquals(c, L"nan") || IEquals(c, L"n/a") ||
           IEquals(c, L"#n/a") || IEquals(c, L"null") || IEquals(c, L"none") ||
           IEquals(c, L"#value!") || IEquals(c, L"#div/0!");
}

bool ParseNumber(Cell c, wchar_t decimal, double* out) {
    const wchar_t thousands = (decimal == L'.') ? L',' : L'.';
    char buf[80];
    size_t n = 0;
    size_t i = 0;
    if (c.empty()) return false;
    if (c[i] == L'+' || c[i] == L'-') {
        if (c[i] == L'-') buf[n++] = '-';
        ++i;
    }
    int int_digits = 0;    // digits in the integer part
    int group = -1;        // digits since the last thousands separator; -1 = none seen
    bool mantissa = false;
    while (i < c.size() && n < sizeof(buf) - 16) {
        const wchar_t ch = c[i];
        if (IsDigit(ch)) {
            buf[n++] = static_cast<char>(ch);
            ++int_digits;
            if (group >= 0) ++group;
            mantissa = true;
            ++i;
        } else if ((ch == thousands || ch == L' ' || ch == L'\'' || ch == 0x00A0 ||
                    ch == 0x202F) &&
                   int_digits > 0) {
            // Groups must be exactly three digits; the first at most three.
            if (group >= 0 ? group != 3 : int_digits > 3) return false;
            group = 0;
            ++i;
        } else {
            break;
        }
    }
    if (group >= 0 && group != 3) return false;
    if (i < c.size() && c[i] == decimal) {
        buf[n++] = '.';
        ++i;
        while (i < c.size() && IsDigit(c[i]) && n < sizeof(buf) - 8) {
            buf[n++] = static_cast<char>(c[i]);
            mantissa = true;
            ++i;
        }
    }
    if (!mantissa) return false;
    if (i < c.size() && (c[i] == L'e' || c[i] == L'E')) {
        size_t j = i + 1;
        char ebuf[8];
        size_t en = 0;
        if (j < c.size() && (c[j] == L'+' || c[j] == L'-')) ebuf[en++] = static_cast<char>(c[j++]);
        size_t digits = 0;
        while (j < c.size() && IsDigit(c[j]) && en < sizeof(ebuf)) {
            ebuf[en++] = static_cast<char>(c[j++]);
            ++digits;
        }
        if (digits == 0) return false;
        buf[n++] = 'e';
        for (size_t k = 0; k < en; ++k) buf[n++] = ebuf[k];
        i = j;
    }
    if (i != c.size()) return false;
    double v = 0.0;
    const auto r = std::from_chars(buf, buf + n, v);
    if (r.ec != std::errc() || r.ptr != buf + n) return false;
    *out = v;
    return true;
}

void NumberHints::Observe(Cell c) {
    int dots = 0, commas = 0;
    size_t last_sep = Cell::npos;
    for (size_t i = 0; i < c.size(); ++i) {
        const wchar_t ch = c[i];
        if (ch == L'.') { ++dots; last_sep = i; }
        else if (ch == L',') { ++commas; last_sep = i; }
        else if (!(IsDigit(ch) || ch == L'-' || ch == L'+' || ch == L'e' || ch == L'E' ||
                   IsSpace(ch))) {
            return;   // not a number
        }
    }
    if (dots + commas == 0) return;
    if (dots > 0 && commas > 0) {
        (c[last_sep] == L'.' ? dot : comma) += 2;
        return;
    }
    if (dots > 1) { comma += 2; return; }    // 1.234.567
    if (commas > 1) { dot += 2; return; }    // 1,234,567
    size_t after = 0;
    for (size_t i = last_sep + 1; i < c.size() && IsDigit(c[i]); ++i) ++after;
    if (after == 3) {
        // Ambiguous ("1.500") unless the integer part is a lone 0 or longer than three digits.
        size_t before = 0;
        bool zero = true;
        for (size_t i = last_sep; i > 0 && IsDigit(c[i - 1]); --i) {
            ++before;
            if (c[i - 1] != L'0') zero = false;
        }
        if (before > 3 || (before == 1 && zero)) (dots ? dot : comma) += 1;
        return;
    }
    (dots ? dot : comma) += 2;
}

wchar_t NumberHints::Decimal(wchar_t delimiter) const {
    if (delimiter == L',') return L'.';
    if (comma != dot) return (comma > dot) ? L',' : L'.';
    // Tie: a semicolon delimiter implies a decimal comma.
    return (delimiter == L';') ? L',' : L'.';
}

long long DaysFromCivil(int y, int m, int d) {
    // Howard Hinnant's days_from_civil.
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

bool ParseDateTime(Cell c, DateOrder order, DateTimeValue* out) {
    if (c.size() < 4) return false;
    size_t i = 0;
    DateTimeValue v;

    // Time on its own?
    {
        size_t j = 0;
        double t = 0.0;
        if (ReadTime(c, &j, &t)) {
            SkipZone(c, &j);
            SkipSpaces(c, &j);
            if (j == c.size()) {
                v.seconds = t;
                v.has_time = true;
                *out = v;
                return true;
            }
        }
    }

    int a = 0, b = 0, y = 0, da = 0, dy = 0;
    if (!ReadInt(c, &i, 4, &a, &da)) return false;
    if (i >= c.size()) return false;
    const wchar_t sep = c[i];
    if (sep != L'-' && sep != L'/' && sep != L'.') return false;
    ++i;
    if (!ReadInt(c, &i, 2, &b)) return false;
    if (i >= c.size() || c[i] != sep) return false;
    ++i;
    int yy = 0, mm = 0, dd = 0;
    if (da == 4) {                         // year first: always Y-M-D
        yy = a;
        mm = b;
        int dd_digits = 0;
        if (!ReadInt(c, &i, 2, &dd, &dd_digits)) return false;
    } else {
        if (da > 2) return false;
        if (!ReadInt(c, &i, 4, &y, &dy)) return false;
        if (dy == 2) y += (y < 70) ? 2000 : 1900;
        else if (dy != 4) return false;
        yy = y;
        if (order == DateOrder::DMY) { dd = a; mm = b; }
        else                         { mm = a; dd = b; }
    }
    if (mm < 1 || mm > 12 || dd < 1 || dd > 31 || yy < 1900 || yy > 2200) return false;
    v.has_date = true;
    v.seconds = static_cast<double>(DaysFromCivil(yy, mm, dd)) * 86400.0;

    if (i < c.size()) {
        // Date and time are separated by 'T' or by spaces.
        size_t j = i;
        if (c[j] == L'T' || c[j] == L't') ++j;
        else if (IsSpace(c[j])) SkipSpaces(c, &j);
        else return false;
        double t = 0.0;
        if (!ReadTime(c, &j, &t)) return false;
        v.seconds += t;
        v.has_time = true;
        SkipZone(c, &j);
        SkipSpaces(c, &j);
        if (j != c.size()) return false;
    }
    *out = v;
    return true;
}

void DateHints::Observe(Cell c) {
    size_t i = 0;
    int a = 0, b = 0, da = 0;
    if (!ReadInt(c, &i, 4, &a, &da) || da > 2 || i >= c.size()) return;
    const wchar_t sep = c[i];
    if (sep != L'-' && sep != L'/' && sep != L'.') return;
    ++i;
    if (!ReadInt(c, &i, 2, &b) || i >= c.size() || c[i] != sep) return;
    if (a > max_first) max_first = a;
    if (b > max_second) max_second = b;
}

DateOrder DateHints::Order() const {
    if (max_first > 12) return DateOrder::DMY;
    if (max_second > 12) return DateOrder::MDY;
    return DateOrder::DMY;
}

std::wstring FormatDateTime(double seconds, bool with_date, double resolution) {
    if (!std::isfinite(seconds)) return L"";
    const double day_f = std::floor(seconds / 86400.0);
    double rem = seconds - day_f * 86400.0;
    // Round to the shown precision before splitting into fields.
    int decimals = 0;
    if (resolution < 0.0095) decimals = 3;
    else if (resolution < 0.095) decimals = 2;
    else if (resolution < 0.95) decimals = 1;
    const double q = std::pow(10.0, decimals);
    rem = std::round(rem * q) / q;
    long long days = static_cast<long long>(day_f);
    if (rem >= 86400.0) { rem -= 86400.0; ++days; }
    const int h = static_cast<int>(rem / 3600.0);
    const int m = static_cast<int>((rem - h * 3600.0) / 60.0);
    const double s = rem - h * 3600.0 - m * 60.0;

    std::wstring time;
    if (resolution >= 59.5 && decimals == 0 && s < 0.5) time = Fmt(L"%02d:%02d", h, m);
    else if (decimals == 0) time = Fmt(L"%02d:%02d:%02d", h, m, static_cast<int>(s + 0.5) % 60);
    else time = Fmt(L"%02d:%02d:%0*.*f", h, m, decimals + 3, decimals, s);
    if (!with_date) return time;

    // civil_from_days.
    long long z = days + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned mo = mp < 10 ? mp + 3 : mp - 9;
    const long long y = static_cast<long long>(yoe) + era * 400 + (mo <= 2);
    return Fmt(L"%04lld-%02u-%02u %s", y, mo, d, time.c_str());
}

}  // namespace core
