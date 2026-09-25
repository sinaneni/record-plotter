// A record file as a table of columns, and the layout detector that reads it.
// The detector:
//   - tries each candidate layout on a sample and scores row consistency and clean cells,
//   - skips a preamble and recognises a header row and a units row,
//   - merges separate date and time columns,
//   - picks the X axis column.
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "core/text_decode.h"

namespace core {

enum class LayoutKind { Delimited, Whitespace, FixedWidth };

// A fixed-width column, as the half-open range of character positions [begin, end).
struct Span {
    int begin = 0;
    int end = 0;
};

struct Layout {
    LayoutKind        kind = LayoutKind::Delimited;
    wchar_t           delimiter = L',';     // Delimited only
    std::vector<Span> spans;                // FixedWidth only
    wchar_t           decimal = L'.';
    size_t            preamble_lines = 0;   // non-blank lines skipped before the table
    bool              has_header = false;
    size_t            unit_rows = 0;        // rows under the header folded into the names
    double            score = 0.0;          // 0..1, how convincing the winner was
};

enum class ColumnKind {
    Number,
    DateTime,    // a date, optionally with a time: seconds since 1970 in the file's clock
    Time,        // a time of day only: seconds since midnight, unwrapped across midnight
    Text,
    Empty,
};

struct Column {
    std::wstring        name;
    ColumnKind          kind = ColumnKind::Empty;
    std::vector<double> values;             // NaN where the cell was missing; empty for Text
    bool                merged_away = false;   // a Time column folded into the date before it
    bool                monotonic = false;  // never decreases (ignoring gaps)
    // A numeric axis column (epoch or running time), offered for X but not as a series.
    bool               axis_like = false;
    size_t              missing = 0;
    double              lo = 0.0, hi = 0.0;   // smallest and largest value, gaps ignored
};

struct Table {
    std::vector<Column> columns;
    size_t              rows = 0;
    Layout              layout;
    Encoding            encoding = Encoding::Utf8;
    UINT                codepage = CP_UTF8;
    size_t              skipped_rows = 0;   // lines inside the table that held no data
    // The column the X axis should start on, or -1 for the row number.
    int                 x_column = -1;
    // Indices of the columns that can be plotted as series, in file order.
    std::vector<int>    series;
};

// Detects the layout of `text` and parses it; false with a reason if nothing is tabular.
bool ParseTable(const std::wstring& text, Table* out, std::wstring* error);

// Reads, decodes and parses a file.
bool LoadTableFile(const std::wstring& path, Table* out, std::wstring* error);

// Detects the layout from the first lines of `text` without parsing the rest.
bool DetectLayout(const std::wstring& text, Layout* out);

// Chooses which series go on the right-hand Y axis, given each one's [lo, hi].
// Empty result means all on the left.
std::vector<bool> ChooseAxes(const std::vector<std::pair<double, double>>& ranges);

// A one-line description of the layout for the UI.
std::wstring DescribeLayout(const Layout& l);

}  // namespace core
