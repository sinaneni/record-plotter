// Parsing of single cells: numbers, dates/times and missing values.
#pragma once

#include <string>
#include <string_view>

namespace core {

using Cell = std::wstring_view;

// Strips surrounding spaces and one pair of surrounding double quotes.
Cell Trim(Cell c);

// True for empty cells and common "no value" spellings such as "-", "NaN", "N/A".
bool IsMissing(Cell c);

// Parses a whole cell as a number with the given decimal separator.
// Thousands separators are accepted only in groups of three.
bool ParseNumber(Cell c, wchar_t decimal, double* out);

// Votes on whether '.' or ',' is the decimal separator from sample cells.
struct NumberHints {
    int dot = 0;
    int comma = 0;
    void Observe(Cell c);
    // The winning separator; never ',' when the delimiter is a comma.
    wchar_t Decimal(wchar_t delimiter) const;
};

// ---------------------------------------------------------------- date/time
// Day/month order for dates with the year last.
enum class DateOrder { DMY, MDY };

struct DateTimeValue {
    double seconds = 0.0;     // since 1970-01-01 in the file's clock, or since midnight
    bool   has_date = false;
    bool   has_time = false;
};

// Parses a date, a date and time, or a time alone; time zone suffixes are ignored.
bool ParseDateTime(Cell c, DateOrder order, DateTimeValue* out);

// Votes on day-first or month-first order from a column's cells; defaults to day-first.
struct DateHints {
    int  max_first = 0;
    int  max_second = 0;
    void Observe(Cell c);
    DateOrder Order() const;
};

// Formats a timestamp with precision matching `resolution` (seconds per step).
std::wstring FormatDateTime(double seconds, bool with_date, double resolution);

// Days from 1970-01-01 to the given civil date.
long long DaysFromCivil(int y, int m, int d);

}  // namespace core
