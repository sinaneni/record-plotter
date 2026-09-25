#include "core/table.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <limits>
#include <map>

#include "core/util.h"
#include "core/values.h"

namespace core {
namespace {

// Number of non-blank lines the detector samples.
constexpr size_t kSampleLines = 1000;

const double kNaN = std::numeric_limits<double>::quiet_NaN();

bool IsBlank(Cell line) {
    for (wchar_t c : line) {
        if (c != L' ' && c != L'\t' && c != L'\r' && c != 0xFEFF) return false;
    }
    return true;
}

// Collects the non-blank lines of `text` as views, up to `limit`.
void SplitLines(const std::wstring& text, size_t limit, std::vector<Cell>* out) {
    out->clear();
    const Cell all(text);
    size_t b = 0;
    const size_t n = all.size();
    while (b < n && out->size() < limit) {
        size_t e = b;
        while (e < n && all[e] != L'\n' && all[e] != L'\r') ++e;
        const Cell line = all.substr(b, e - b);
        if (!IsBlank(line)) out->push_back(line);
        if (e < n && all[e] == L'\r' && e + 1 < n && all[e + 1] == L'\n') ++e;
        b = e + 1;
    }
}

// ------------------------------------------------------------------ splitters
// Splits a line on a delimiter, honouring double quotes.
void SplitDelimited(Cell line, wchar_t delim, std::vector<Cell>* f) {
    f->clear();
    size_t i = 0;
    const size_t n = line.size();
    for (;;) {
        const size_t start = i;
        size_t j = i;
        while (j < n && (line[j] == L' ' || line[j] == L'\t') && line[j] != delim) ++j;
        if (j < n && line[j] == L'"') {
            ++j;
            while (j < n) {
                if (line[j] == L'"') {
                    if (j + 1 < n && line[j + 1] == L'"') { j += 2; continue; }
                    ++j;
                    break;
                }
                ++j;
            }
        }
        while (j < n && line[j] != delim) ++j;
        f->push_back(line.substr(start, j - start));
        if (j >= n) break;
        i = j + 1;
    }
}

// Splits a line on runs of spaces and tabs.
void SplitWhitespace(Cell line, std::vector<Cell>* f) {
    f->clear();
    size_t i = 0;
    const size_t n = line.size();
    while (i < n) {
        while (i < n && (line[i] == L' ' || line[i] == L'\t')) ++i;
        if (i >= n) break;
        size_t j = i;
        while (j < n && line[j] != L' ' && line[j] != L'\t') ++j;
        f->push_back(line.substr(i, j - i));
        i = j;
    }
}

// Splits a line into fixed-width columns, cutting at the first blank after each span's end
// so values wider than the sample are kept whole.
void SplitFixed(Cell line, const std::vector<Span>& spans, std::vector<Cell>* f) {
    f->clear();
    const int n = static_cast<int>(line.size());
    int from = 0;
    for (size_t k = 0; k < spans.size(); ++k) {
        int cut = n;   // the last column runs to the end of the line
        if (k + 1 < spans.size()) {
            const int limit = (std::min)(spans[k + 1].begin, n);
            cut = limit;
            for (int q = (std::max)(spans[k].end, from); q < limit; ++q) {
                if (line[static_cast<size_t>(q)] == L' ' || line[static_cast<size_t>(q)] == L'\t') {
                    cut = q;
                    break;
                }
            }
        }
        if (from >= n || cut <= from) {
            f->push_back(Cell());
        } else {
            f->push_back(line.substr(static_cast<size_t>(from), static_cast<size_t>(cut - from)));
        }
        from = (std::max)(from, cut);
    }
}

// Splits a line with the given layout and trims each cell.
void SplitWith(const Layout& l, Cell line, std::vector<Cell>* f) {
    switch (l.kind) {
        case LayoutKind::Delimited:  SplitDelimited(line, l.delimiter, f); break;
        case LayoutKind::Whitespace: SplitWhitespace(line, f); break;
        case LayoutKind::FixedWidth: SplitFixed(line, l.spans, f); break;
    }
    for (Cell& c : *f) c = Trim(c);
}

// ------------------------------------------------------------ cell classes
enum class CellClass { Missing, Number, Date, DateTimeC, Time, Text };

CellClass Classify(Cell c, wchar_t decimal, DateOrder order) {
    if (IsMissing(c)) return CellClass::Missing;
    double v = 0.0;
    if (ParseNumber(c, decimal, &v)) return CellClass::Number;
    DateTimeValue dt;
    if (ParseDateTime(c, order, &dt)) {
        if (!dt.has_date) return CellClass::Time;
        return dt.has_time ? CellClass::DateTimeC : CellClass::Date;
    }
    return CellClass::Text;
}

bool Fits(ColumnKind kind, CellClass cc) {
    switch (kind) {
        case ColumnKind::Number:   return cc == CellClass::Number;
        case ColumnKind::DateTime: return cc == CellClass::Date || cc == CellClass::DateTimeC;
        case ColumnKind::Time:     return cc == CellClass::Time;
        default:                   return false;
    }
}

bool Typed(ColumnKind k) {
    return k == ColumnKind::Number || k == ColumnKind::DateTime || k == ColumnKind::Time;
}

// What the rows of one candidate say about their columns.
struct Analysis {
    std::vector<ColumnKind> kinds;
    std::vector<DateOrder>  orders;
    wchar_t                 decimal = L'.';
    bool                    has_header = false;
    size_t                  unit_rows = 0;
    double                  clean = 0.0;   // mean share of well-typed cells per column
};

// Infers column kinds, decimal separator, header and unit rows from split sample rows.
void Analyze(const std::vector<std::vector<Cell>>& rows, size_t k, wchar_t delimiter,
             Analysis* a) {
    a->kinds.assign(k, ColumnKind::Empty);
    a->orders.assign(k, DateOrder::DMY);
    a->has_header = false;
    a->unit_rows = 0;
    a->clean = 0.0;
    if (rows.empty() || k == 0) return;

    // Kinds are decided on body rows only, skipping possible header and unit rows.
    const size_t body = rows.size() > 8 ? 4 : (rows.size() > 1 ? 1 : 0);

    NumberHints nh;
    std::vector<DateHints> dh(k);
    for (size_t r = body; r < rows.size(); ++r) {
        for (size_t j = 0; j < k && j < rows[r].size(); ++j) {
            nh.Observe(rows[r][j]);
            dh[j].Observe(rows[r][j]);
        }
    }
    a->decimal = nh.Decimal(delimiter);

    double clean_sum = 0.0;
    size_t clean_cols = 0;
    for (size_t j = 0; j < k; ++j) {
        a->orders[j] = dh[j].Order();
        size_t num = 0, date = 0, time = 0, present = 0;
        for (size_t r = body; r < rows.size(); ++r) {
            if (j >= rows[r].size()) continue;
            const CellClass cc = Classify(rows[r][j], a->decimal, a->orders[j]);
            if (cc == CellClass::Missing) continue;
            ++present;
            if (cc == CellClass::Number) ++num;
            else if (cc == CellClass::Date || cc == CellClass::DateTimeC) ++date;
            else if (cc == CellClass::Time) ++time;
        }
        if (present == 0) { a->kinds[j] = ColumnKind::Empty; continue; }
        const size_t best = (std::max)(num, (std::max)(date, time));
        // At least 90% of present cells must share a type.
        if (best * 10 >= present * 9) {
            a->kinds[j] = (best == num) ? ColumnKind::Number
                        : (best == date) ? ColumnKind::DateTime
                                         : ColumnKind::Time;
        } else {
            a->kinds[j] = ColumnKind::Text;
        }
        clean_sum += static_cast<double>(best) / static_cast<double>(present);
        ++clean_cols;
    }
    a->clean = clean_cols ? clean_sum / static_cast<double>(clean_cols) : 0.0;

    // Header: a first row that does not fit the typed columns.
    size_t typed = 0, votes = 0;
    for (size_t j = 0; j < k; ++j) {
        if (!Typed(a->kinds[j])) continue;
        ++typed;
        if (j < rows[0].size()) {
            const CellClass cc = Classify(rows[0][j], a->decimal, a->orders[j]);
            if (cc != CellClass::Missing && !Fits(a->kinds[j], cc)) ++votes;
        }
    }
    if (typed > 0) {
        a->has_header = votes > 0 && votes * 2 >= typed;
    } else {
        // No typed columns: a fully filled first row is taken as the header.
        bool words_only = true;
        for (Cell c : rows[0]) {
            if (Classify(c, a->decimal, DateOrder::DMY) != CellClass::Text) words_only = false;
        }
        a->has_header = !rows[0].empty() && (rows.size() > 1 || words_only);
        for (Cell c : rows[0]) {
            if (IsMissing(c)) a->has_header = false;
        }
    }

    // Unit rows: rows under the header that still do not fit the typed columns.
    if (a->has_header && typed > 0) {
        for (size_t r = 1; r < rows.size() && r <= 3 && r < body + 1; ++r) {
            size_t miss = 0, fit = 0;
            for (size_t j = 0; j < k; ++j) {
                if (!Typed(a->kinds[j]) || j >= rows[r].size()) continue;
                const CellClass cc = Classify(rows[r][j], a->decimal, a->orders[j]);
                if (Fits(a->kinds[j], cc)) ++fit;
                else if (cc != CellClass::Missing) ++miss;
            }
            if (fit == 0 && miss > 0) ++a->unit_rows;
            else break;
        }
    }
}

// ---------------------------------------------------------------- candidates
struct Candidate {
    Layout   layout;
    Analysis analysis;
    double   score = 0.0;
};

// The most common field count over the latter half of the sample.
size_t ModalCount(const std::vector<size_t>& counts) {
    const size_t from = counts.size() >= 4 ? counts.size() / 2 : 0;
    std::map<size_t, size_t> freq;
    for (size_t i = from; i < counts.size(); ++i) ++freq[counts[i]];
    size_t best = 0, best_n = 0;
    for (const auto& kv : freq) {
        if (kv.second > best_n || (kv.second == best_n && kv.first > best)) {
            best = kv.first;
            best_n = kv.second;
        }
    }
    return best;
}

// The first line where `ok` holds for it and the next two lines.
size_t TableStart(size_t n, const std::vector<bool>& ok) {
    for (size_t i = 0; i < n; ++i) {
        bool run = true;
        for (size_t j = i; j < n && j < i + 3; ++j) run = run && ok[j];
        if (run && ok[i]) return i;
    }
    return n;
}

double Score(double consistency, const Analysis& a) {
    return consistency * (0.4 + 0.6 * a.clean);
}

// Splits a header on tabs or runs of two or more spaces, keeping multi-word names whole.
std::vector<Cell> HeaderWords(Cell h) {
    std::vector<Cell> words;
    size_t i = 0;
    while (i < h.size()) {
        while (i < h.size() && (h[i] == L' ' || h[i] == L'\t')) ++i;
        if (i >= h.size()) break;
        size_t j = i;
        while (j < h.size()) {
            if (h[j] == L'\t') break;
            if (h[j] == L' ' && (j + 1 >= h.size() || h[j + 1] == L' ' || h[j + 1] == L'\t')) break;
            ++j;
        }
        words.push_back(Trim(h.substr(i, j - i)));
        i = j;
    }
    return words;
}

// Start index of each logical column, treating a date followed by a time as one.
std::vector<size_t> LogicalStarts(const std::vector<ColumnKind>& kinds) {
    std::vector<size_t> starts;
    for (size_t j = 0; j < kinds.size(); ++j) {
        starts.push_back(j);
        if (kinds[j] == ColumnKind::DateTime && j + 1 < kinds.size() &&
            kinds[j + 1] == ColumnKind::Time) {
            ++j;
        }
    }
    return starts;
}

// True if `line` holds one mostly non-numeric name per column.
bool NamesColumns(Cell line, const Analysis& a) {
    const std::vector<Cell> words = HeaderWords(line);
    if (words.size() < 2) return false;
    if (words.size() != LogicalStarts(a.kinds).size() && words.size() != a.kinds.size()) {
        return false;
    }
    size_t text = 0;
    for (Cell w : words) {
        double v = 0.0;
        if (!ParseNumber(w, a.decimal, &v)) ++text;
    }
    return text * 2 > words.size();
}

// Scores a delimited or whitespace candidate by its field-count consistency.
void EvaluateSplit(const std::vector<Cell>& sample, Candidate* c) {
    const size_t n = sample.size();
    std::vector<std::vector<Cell>> split(n);
    std::vector<size_t> counts(n);
    for (size_t i = 0; i < n; ++i) {
        SplitWith(c->layout, sample[i], &split[i]);
        counts[i] = split[i].size();
    }
    const size_t k = ModalCount(counts);
    if (k < 2) return;
    std::vector<bool> ok(n);
    for (size_t i = 0; i < n; ++i) ok[i] = counts[i] == k;
    const size_t start = TableStart(n, ok);
    if (start >= n) return;
    size_t good = 0;
    for (size_t i = start; i < n; ++i) good += ok[i] ? 1 : 0;
    const double consistency = static_cast<double>(good) / static_cast<double>(n - start);

    std::vector<std::vector<Cell>> rows(split.begin() + static_cast<std::ptrdiff_t>(start),
                                        split.end());
    Analyze(rows, k, c->layout.kind == LayoutKind::Delimited ? c->layout.delimiter : 0,
            &c->analysis);
    // A whitespace header with multi-word names is the line just above the table.
    size_t first = start;
    if (c->layout.kind == LayoutKind::Whitespace && !c->analysis.has_header && start > 0 &&
        NamesColumns(sample[start - 1], c->analysis)) {
        --first;
        c->analysis.has_header = true;
    }
    c->layout.preamble_lines = first;
    c->score = Score(consistency, c->analysis);
}

// Scores a fixed-width candidate, finding columns from positions blank on every line.
void EvaluateFixed(const std::vector<Cell>& sample, Candidate* c) {
    const size_t n = sample.size();
    if (n < 2) return;
    // Profile the lower part of the sample, skipping preamble and header.
    const size_t from = n >= 10 ? n * 3 / 10 : 1;
    size_t width = 0;
    for (size_t i = from; i < n; ++i) width = (std::max)(width, sample[i].size());
    if (width < 3) return;
    std::vector<size_t> ink(width, 0), digit(width, 0), alpha(width, 0);
    for (size_t i = from; i < n; ++i) {
        const Cell l = sample[i];
        for (size_t p = 0; p < l.size(); ++p) {
            const wchar_t ch = l[p];
            if (ch == L' ' || ch == L'\t') continue;
            ++ink[p];
            if (ch >= L'0' && ch <= L'9') ++digit[p];
            else if ((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') || ch > 0x7F) ++alpha[p];
        }
    }
    // Tolerate one stray character per 200 lines in a gap.
    const size_t tolerance = (n - from) / 200;
    std::vector<Span> spans;
    for (size_t p = 0; p < width;) {
        if (ink[p] <= tolerance) { ++p; continue; }
        size_t e = p;
        while (e < width && ink[e] > tolerance) ++e;
        spans.push_back({static_cast<int>(p), static_cast<int>(e)});
        p = e;
    }

    // Split packed columns where a digit run meets a letter run (each side two or more wide).
    auto cls = [&](size_t p) -> int {   // 1 digit, 2 letter, 0 mixed
        if (ink[p] == 0) return 0;
        if (digit[p] * 20 >= ink[p] * 19) return 1;
        if (alpha[p] * 20 >= ink[p] * 19) return 2;
        return 0;
    };
    std::vector<Span> split_spans;
    for (const Span& s : spans) {
        int b = s.begin;
        for (int p = s.begin + 1; p + 1 < s.end; ++p) {
            const int l = cls(static_cast<size_t>(p)), r = cls(static_cast<size_t>(p + 1));
            if (l == 0 || r == 0 || l == r) continue;
            if (p - b < 1) continue;
            const int lp = cls(static_cast<size_t>(p - 1));
            const int rn = cls(static_cast<size_t>(p + 2 < s.end ? p + 2 : p + 1));
            if (lp == l && rn == r) {
                split_spans.push_back({b, p + 1});
                b = p + 1;
            }
        }
        split_spans.push_back({b, s.end});
    }
    spans.swap(split_spans);
    if (spans.size() < 2) return;
    c->layout.spans = spans;

    // A line fits when all of its ink falls inside the columns.
    std::vector<bool> fit(n);
    for (size_t i = 0; i < n; ++i) {
        bool f = true;
        const Cell l = sample[i];
        size_t k = 0;
        for (size_t p = 0; p < l.size() && f; ++p) {
            if (l[p] == L' ' || l[p] == L'\t') continue;
            while (k < spans.size() && static_cast<size_t>(spans[k].end) <= p) ++k;
            // The last column is open-ended.
            if (k >= spans.size()) { f = (p >= static_cast<size_t>(spans.back().begin)); break; }
            if (p < static_cast<size_t>(spans[k].begin)) f = false;
        }
        fit[i] = f;
    }
    size_t start = TableStart(n, fit);
    if (start >= n) return;
    size_t good = 0;
    for (size_t i = start; i < n; ++i) good += fit[i] ? 1 : 0;
    const double consistency = static_cast<double>(good) / static_cast<double>(n - start);

    std::vector<std::vector<Cell>> rows(n - start);
    auto split_rows = [&] {
        for (size_t i = start; i < n; ++i) SplitWith(c->layout, sample[i], &rows[i - start]);
        Analyze(rows, c->layout.spans.size(), 0, &c->analysis);
    };
    split_rows();

    // Rejoin adjacent text columns one blank apart (text containing spaces).
    bool merged = false;
    std::vector<Span>& sp = c->layout.spans;
    for (size_t j = 0; j + 1 < sp.size();) {
        if (c->analysis.kinds[j] == ColumnKind::Text &&
            c->analysis.kinds[j + 1] == ColumnKind::Text && sp[j + 1].begin - sp[j].end <= 1) {
            sp[j].end = sp[j + 1].end;
            sp.erase(sp.begin() + static_cast<std::ptrdiff_t>(j + 1));
            c->analysis.kinds.erase(c->analysis.kinds.begin() + static_cast<std::ptrdiff_t>(j + 1));
            merged = true;
        } else {
            ++j;
        }
    }
    if (merged) split_rows();
    if (sp.size() < 2) { c->score = 0.0; return; }

    // A header wider than its columns is the non-numeric line just above the table.
    if (!c->analysis.has_header && start > 0) {
        std::vector<Cell> words;
        SplitWhitespace(sample[start - 1], &words);
        size_t text = 0;
        for (Cell w : words) {
            double v = 0.0;
            if (!ParseNumber(Trim(w), c->analysis.decimal, &v)) ++text;
        }
        if (words.size() >= 2 && text * 2 > words.size()) {
            --start;
            c->analysis.has_header = true;
        }
    }
    c->layout.preamble_lines = start;
    c->score = Score(consistency, c->analysis);
}

// Assigns each header word to the fixed-width column it overlaps most, or the nearest.
std::vector<std::wstring> FixedHeaderNames(Cell header, const std::vector<Span>& spans) {
    std::vector<std::wstring> names(spans.size());
    size_t i = 0;
    const size_t n = header.size();
    while (i < n) {
        while (i < n && (header[i] == L' ' || header[i] == L'\t')) ++i;
        if (i >= n) break;
        size_t j = i;
        while (j < n && header[j] != L' ' && header[j] != L'\t') ++j;
        const int b = static_cast<int>(i), e = static_cast<int>(j);
        int best = -1, best_overlap = 0, nearest = 0;
        double nearest_d = 1e18;
        for (size_t k = 0; k < spans.size(); ++k) {
            const int se = (k + 1 == spans.size()) ? (std::max)(spans[k].end, e) : spans[k].end;
            const int ov = (std::min)(e, se) - (std::max)(b, spans[k].begin);
            if (ov > best_overlap) { best_overlap = ov; best = static_cast<int>(k); }
            const double d = std::fabs((b + e) * 0.5 - (spans[k].begin + se) * 0.5);
            if (d < nearest_d) { nearest_d = d; nearest = static_cast<int>(k); }
        }
        const size_t k = static_cast<size_t>(best >= 0 ? best : nearest);
        if (!names[k].empty()) names[k] += L' ';
        names[k].append(header.substr(i, j - i));
        i = j;
    }
    return names;
}

std::wstring CleanName(Cell c) {
    const Cell t = Trim(c);
    std::wstring s;
    s.reserve(t.size());
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == L'"' && i + 1 < t.size() && t[i + 1] == L'"') ++i;   // "" -> "
        s += t[i];
    }
    return s;
}

// Evaluates every candidate layout and keeps the best-scoring one.
bool Detect(const std::vector<Cell>& sample, Candidate* best) {
    // Candidate order breaks ties: earlier wins.
    std::vector<Candidate> cands;
    for (wchar_t d : {L',', L';', L'\t', L'|'}) {
        Candidate c;
        c.layout.kind = LayoutKind::Delimited;
        c.layout.delimiter = d;
        cands.push_back(c);
    }
    {
        Candidate c;
        c.layout.kind = LayoutKind::FixedWidth;
        cands.push_back(c);
    }
    {
        Candidate c;
        c.layout.kind = LayoutKind::Whitespace;
        cands.push_back(c);
    }
    double top = 0.0;
    int win = -1;
    for (size_t i = 0; i < cands.size(); ++i) {
        Candidate& c = cands[i];
        if (c.layout.kind == LayoutKind::FixedWidth) EvaluateFixed(sample, &c);
        else EvaluateSplit(sample, &c);
        if (c.score > top + 1e-9) { top = c.score; win = static_cast<int>(i); }
    }
    if (win < 0) return false;
    *best = cands[static_cast<size_t>(win)];
    best->layout.decimal = best->analysis.decimal;
    best->layout.has_header = best->analysis.has_header;
    best->layout.unit_rows = best->analysis.unit_rows;
    best->layout.score = best->score;
    return true;
}

// Local time zone offset in seconds at the given Unix time.
double LocalOffset(double unix_s) {
    const double ticks = (unix_s + 11644473600.0) * 1e7;
    if (!(ticks > 0.0)) return 0.0;
    ULARGE_INTEGER u;
    u.QuadPart = static_cast<ULONGLONG>(ticks);
    FILETIME ft = {u.LowPart, u.HighPart};
    SYSTEMTIME utc = {}, local = {};
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local) ||
        !SystemTimeToFileTime(&local, &ft)) {
        return 0.0;
    }
    ULARGE_INTEGER l;
    l.LowPart = ft.dwLowDateTime;
    l.HighPart = ft.dwHighDateTime;
    return std::round((static_cast<double>(l.QuadPart) - static_cast<double>(u.QuadPart)) / 1e7);
}

// True for column names that suggest a time or index axis.
bool NameLooksLikeX(const std::wstring& name) {
    std::wstring s;
    for (wchar_t c : name) s += static_cast<wchar_t>(towlower(c));
    static const wchar_t* kWords[] = {L"time", L"zaman", L"sure", L"süre", L"t", L"sec", L"epoch",
                                      L"s", L"ms", L"index", L"idx", L"no", L"sample",
                                      L"#", L"counter", L"sayaç", L"step", L"adım", L"n"};
    for (const wchar_t* w : kWords) {
        if (s == w) return true;
    }
    return s.find(L"time") != std::wstring::npos || s.find(L"zaman") != std::wstring::npos ||
           s.find(L"epoch") != std::wstring::npos || s.find(L"unix") != std::wstring::npos ||
           s.find(L"[s]") != std::wstring::npos || s.find(L"(s)") != std::wstring::npos;
}

}  // namespace

bool DetectLayout(const std::wstring& text, Layout* out) {
    std::vector<Cell> sample;
    SplitLines(text, kSampleLines, &sample);
    Candidate best;
    if (!Detect(sample, &best)) return false;
    *out = best.layout;
    return true;
}

bool ParseTable(const std::wstring& text, Table* out, std::wstring* error) {
    *out = Table();
    std::vector<Cell> lines;
    SplitLines(text, static_cast<size_t>(-1), &lines);
    if (lines.empty()) {
        if (error) *error = L"The file is empty.";
        return false;
    }
    const std::vector<Cell> sample(lines.begin(),
                                   lines.begin() + static_cast<std::ptrdiff_t>(
                                       (std::min)(lines.size(), kSampleLines)));
    Candidate best;
    if (!Detect(sample, &best)) {
        if (error) {
            *error = L"No table found: the file has no regular lines with at least two "
                     L"columns.";
        }
        return false;
    }
    const Layout& l = best.layout;
    const Analysis& a = best.analysis;
    const size_t k = a.kinds.size();
    out->layout = l;

    // ------------------------------------------------------------ names
    out->columns.resize(k);
    const size_t header_line = l.preamble_lines;
    size_t data_line = l.preamble_lines;
    std::vector<Cell> f;
    if (l.has_header) {
        if (l.kind == LayoutKind::FixedWidth || l.kind == LayoutKind::Whitespace) {
            // Assign names in order when their count matches the columns, else by position.
            const std::vector<Cell> words = HeaderWords(lines[header_line]);
            const std::vector<size_t> starts = LogicalStarts(a.kinds);
            if (words.size() == starts.size()) {
                for (size_t g = 0; g < starts.size(); ++g) {
                    out->columns[starts[g]].name = CleanName(words[g]);
                }
            } else if (words.size() == k) {
                for (size_t j = 0; j < k; ++j) out->columns[j].name = CleanName(words[j]);
            } else if (l.kind == LayoutKind::Whitespace) {
                SplitWith(l, lines[header_line], &f);   // "t x y": one word per column
                for (size_t j = 0; j < k && j < f.size(); ++j) out->columns[j].name = CleanName(f[j]);
            } else {
                const std::vector<std::wstring> names = FixedHeaderNames(lines[header_line], l.spans);
                for (size_t j = 0; j < k; ++j) out->columns[j].name = names[j];
            }
        } else {
            SplitWith(l, lines[header_line], &f);
            for (size_t j = 0; j < k && j < f.size(); ++j) out->columns[j].name = CleanName(f[j]);
        }
        ++data_line;
        for (size_t u = 0; u < l.unit_rows && data_line < lines.size(); ++u, ++data_line) {
            SplitWith(l, lines[data_line], &f);
            for (size_t j = 0; j < k && j < f.size(); ++j) {
                const std::wstring unit = CleanName(f[j]);
                if (unit.empty()) continue;
                if (!out->columns[j].name.empty()) out->columns[j].name += L' ';
                out->columns[j].name += unit;
            }
        }
    }
    std::vector<bool> auto_named(k, false);
    for (size_t j = 0; j < k; ++j) {
        Column& c = out->columns[j];
        c.kind = a.kinds[j];
        if (c.name.empty()) {
            c.name = Fmt(L"Column %zu", j + 1);
            auto_named[j] = true;
        }
    }

    // ------------------------------------------------------------- rows
    const size_t capacity = lines.size() - (std::min)(lines.size(), data_line);
    for (Column& c : out->columns) {
        if (Typed(c.kind)) c.values.reserve(capacity);
    }
    std::vector<bool> date_only(k, true);
    std::vector<double> row(k);
    for (size_t i = data_line; i < lines.size(); ++i) {
        SplitWith(l, lines[i], &f);
        bool any = false;
        for (size_t j = 0; j < k; ++j) {
            row[j] = kNaN;
            const ColumnKind kind = a.kinds[j];
            if (!Typed(kind) || j >= f.size() || IsMissing(f[j])) continue;
            if (kind == ColumnKind::Number) {
                double v = 0.0;
                if (ParseNumber(f[j], a.decimal, &v)) { row[j] = v; any = true; }
            } else {
                DateTimeValue dt;
                if (ParseDateTime(f[j], a.orders[j], &dt) &&
                    (kind == ColumnKind::Time) == !dt.has_date) {
                    row[j] = dt.seconds;
                    if (dt.has_time) date_only[j] = false;
                    any = true;
                }
            }
        }
        // Skip lines with no values, such as footers or repeated headers.
        if (!any) { ++out->skipped_rows; continue; }
        for (size_t j = 0; j < k; ++j) {
            if (Typed(a.kinds[j])) out->columns[j].values.push_back(row[j]);
        }
        ++out->rows;
    }
    if (out->rows == 0) {
        if (error) {
            *error = data_line >= lines.size()
                         ? L"The file has a header but no data rows."
                         : L"A table was found, but it holds no readable numeric values.";
        }
        return false;
    }

    // ------------------------------------------- date + time, and midnight
    for (size_t j = 0; j + 1 < k; ++j) {
        Column& d = out->columns[j];
        Column& t = out->columns[j + 1];
        if (d.kind != ColumnKind::DateTime || !date_only[j] || t.kind != ColumnKind::Time) continue;
        for (size_t r = 0; r < out->rows; ++r) {
            d.values[r] = (std::isnan(d.values[r]) || std::isnan(t.values[r]))
                              ? kNaN : d.values[r] + t.values[r];
        }
        // An auto-named time column does not add to the merged name.
        if (!auto_named[j + 1]) d.name += L' ' + t.name;
        t.merged_away = true;
    }
    for (Column& c : out->columns) {
        if (c.kind != ColumnKind::Time || c.merged_away) continue;
        // Unwrap across midnight: a step back of over half a day is the next day.
        double offset = 0.0, prev = kNaN;
        for (double& v : c.values) {
            if (std::isnan(v)) continue;
            if (!std::isnan(prev) && v + offset < prev - 43200.0) offset += 86400.0;
            v += offset;
            prev = v;
        }
    }

    // --------------------------------------------------------- statistics
    for (Column& c : out->columns) {
        if (!Typed(c.kind)) continue;
        bool mono = true;
        double prev = kNaN;
        c.lo = std::numeric_limits<double>::infinity();
        c.hi = -c.lo;
        for (double v : c.values) {
            if (std::isnan(v)) { ++c.missing; continue; }
            if (!std::isnan(prev) && v < prev) mono = false;
            prev = v;
            if (v < c.lo) c.lo = v;
            if (v > c.hi) c.hi = v;
        }
        if (c.lo > c.hi) c.lo = c.hi = kNaN;   // no values at all
        c.monotonic = mono;
        if (c.kind == ColumnKind::Number && mono && c.missing < c.values.size()) {
            double first = kNaN, last = kNaN;
            for (double v : c.values) {
                if (std::isnan(v)) continue;
                if (std::isnan(first)) first = v;
                last = v;
            }
            // Unix seconds or milliseconds between 1998 and 2100.
            const bool epoch = (first > 9e8 && last < 4.2e9) || (first > 9e11 && last < 4.2e12);
            c.axis_like = last > first && (epoch || NameLooksLikeX(c.name));
            // Convert a Unix time column to a local-time DateTime column.
            if (c.axis_like && epoch) {
                const double scale = first > 9e11 ? 0.001 : 1.0;
                const double off_first = LocalOffset(first * scale);
                const bool same = off_first == LocalOffset(last * scale);
                for (double& v : c.values) {
                    if (std::isnan(v)) continue;
                    const double s = v * scale;
                    // One offset for the whole column unless it crosses a daylight-saving change.
                    v = s + (same ? off_first : LocalOffset(s));
                }
                c.lo = c.values.empty() ? kNaN : first * scale + off_first;
                c.hi = c.values.empty() ? kNaN : last * scale + (same ? off_first : LocalOffset(last * scale));
                c.kind = ColumnKind::DateTime;
            }
        }
    }

    // --------------------------------------------------------- the X axis
    out->x_column = -1;
    for (int pass = 0; pass < 3 && out->x_column < 0; ++pass) {
        for (size_t j = 0; j < k; ++j) {
            const Column& c = out->columns[j];
            if (c.merged_away || c.values.empty()) continue;
            const bool hit =
                (pass == 0 && c.kind == ColumnKind::DateTime) ||
                (pass == 1 && c.kind == ColumnKind::Time) ||
                (pass == 2 && c.kind == ColumnKind::Number && c.monotonic &&
                 c.values.front() < c.values.back() && (j == 0 || NameLooksLikeX(c.name)));
            if (hit) { out->x_column = static_cast<int>(j); break; }
        }
    }
    for (size_t j = 0; j < k; ++j) {
        const Column& c = out->columns[j];
        if (c.kind == ColumnKind::Number && static_cast<int>(j) != out->x_column &&
            !c.axis_like && c.missing < c.values.size()) {
            out->series.push_back(static_cast<int>(j));
        }
    }
    return true;
}

bool LoadTableFile(const std::wstring& path, Table* out, std::wstring* error) {
    std::string bytes;
    if (!ReadFileBytes(path, &bytes, error)) return false;
    DecodedText text;
    DecodeText(bytes, &text);
    std::string().swap(bytes);   // the raw bytes are not needed past this point
    if (!ParseTable(text.text, out, error)) return false;
    out->encoding = text.encoding;
    out->codepage = text.codepage;
    return true;
}

std::vector<bool> ChooseAxes(const std::vector<std::pair<double, double>>& ranges) {
    const size_t n = ranges.size();
    std::vector<bool> none(n, false);
    if (n < 2) return none;
    std::vector<bool> valid(n);
    for (size_t i = 0; i < n; ++i) {
        valid[i] = std::isfinite(ranges[i].first) && std::isfinite(ranges[i].second);
    }

    // The worst readability over the series that move, for one assignment.
    auto worst = [&](const std::vector<bool>& right) {
        double lo[2] = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
        double hi[2] = {-lo[0], -lo[1]};
        for (size_t i = 0; i < n; ++i) {
            if (!valid[i]) continue;
            const int a = right[i] ? 1 : 0;
            lo[a] = (std::min)(lo[a], ranges[i].first);
            hi[a] = (std::max)(hi[a], ranges[i].second);
        }
        double w = 1.0;
        for (size_t i = 0; i < n; ++i) {
            if (!valid[i]) continue;
            const double span = ranges[i].second - ranges[i].first;
            if (!(span > 0.0)) continue;               // constant: cannot be helped
            const int a = right[i] ? 1 : 0;
            w = (std::min)(w, span / (hi[a] - lo[a]));
        }
        return w;
    };

    const double base = worst(none);
    if (base >= 0.2) return none;

    std::vector<bool> best = none;
    double best_w = base;
    size_t best_right = 0;
    double best_mag = 0.0;
    // Tie-break: prefer keeping the larger values on the left axis.
    auto magnitude = [&](const std::vector<bool>& right) {
        double m = 0.0;
        for (size_t i = 0; i < n; ++i) {
            if (right[i] && valid[i]) {
                m = (std::max)(m, (std::max)(std::fabs(ranges[i].first), std::fabs(ranges[i].second)));
            }
        }
        return m;
    };
    auto consider = [&](const std::vector<bool>& right) {
        size_t count = 0;
        for (bool r : right) count += r ? 1 : 0;
        if (count == 0 || count == n) return;          // one axis either way
        const double w = worst(right);
        const double mag = magnitude(right);
        const bool better = w > best_w * (1 + 1e-9);
        const bool as_good = !better && w >= best_w * (1 - 1e-9);
        if (better || (as_good && (count < best_right ||
                                   (count == best_right && mag < best_mag)))) {
            best = right;
            best_w = w;
            best_right = count;
            best_mag = mag;
        }
    };
    std::vector<bool> right(n);
    if (n <= 14) {
        for (unsigned mask = 1; mask < (1u << n); ++mask) {
            for (size_t i = 0; i < n; ++i) right[i] = ((mask >> i) & 1u) != 0;
            consider(right);
        }
    } else {
        // Too many to try all: sort by swing and try each cut point.
        std::vector<size_t> order(n);
        for (size_t i = 0; i < n; ++i) order[i] = i;
        auto span = [&](size_t i) { return valid[i] ? ranges[i].second - ranges[i].first : 0.0; };
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return span(a) < span(b); });
        for (size_t cut = 1; cut < n; ++cut) {
            std::fill(right.begin(), right.end(), false);
            for (size_t k = 0; k < cut; ++k) right[order[k]] = true;
            consider(right);
        }
    }
    if (best_right == 0 || best_w < base * 2.0) return none;
    return best;
}

std::wstring DescribeLayout(const Layout& l) {
    switch (l.kind) {
        case LayoutKind::Delimited:
            switch (l.delimiter) {
                case L',':  return L"CSV, comma separated";
                case L';':  return L"CSV, semicolon separated";
                case L'\t': return L"CSV, tab separated";
                case L'|':  return L"CSV, pipe separated";
                default:    return Fmt(L"CSV, '%c' separated", l.delimiter);
            }
        case LayoutKind::Whitespace:
            return L"Whitespace separated";
        case LayoutKind::FixedWidth:
            return L"Fixed width";
    }
    return L"";
}

}  // namespace core
