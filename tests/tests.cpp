// Tests for the cell parsers, the layout detector and the sample files in tests/samples.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>

#include "core/table.h"
#include "core/util.h"
#include "core/values.h"

namespace {

int g_failed = 0;
int g_checks = 0;

void Report(bool ok, const char* file, int line, const std::wstring& what) {
    ++g_checks;
    if (ok) return;
    ++g_failed;
    std::printf("FAIL %s:%d  %s\n", file, line, core::ToUtf8(what).c_str());
}

#define CHECK(cond) Report((cond), __FILE__, __LINE__, L## #cond)
#define CHECK_EQ(a, b) Report((a) == (b), __FILE__, __LINE__, \
                              core::Fmt(L"%s == %s", L## #a, L## #b))
#define CHECK_NEAR(a, b, eps) Report(std::fabs((a) - (b)) <= (eps), __FILE__, __LINE__, \
                                     core::Fmt(L"%s ~ %s (got %.9g)", L## #a, L## #b, \
                                               static_cast<double>(a)))
#define CHECK_STR(a, b) Report((a) == (b), __FILE__, __LINE__, \
                               core::Fmt(L"%s == \"%s\" (got \"%s\")", L## #a, (b), \
                                         std::wstring(a).c_str()))

double Num(const wchar_t* s, wchar_t dec, bool* ok) {
    double v = 0.0;
    *ok = core::ParseNumber(s, dec, &v);
    return v;
}

void TestNumbers() {
    bool ok = false;
    CHECK_NEAR(Num(L"12.5", L'.', &ok), 12.5, 1e-12); CHECK(ok);
    CHECK_NEAR(Num(L"-12,5", L',', &ok), -12.5, 1e-12); CHECK(ok);
    CHECK_NEAR(Num(L"1.234,5", L',', &ok), 1234.5, 1e-9); CHECK(ok);
    CHECK_NEAR(Num(L"1,234,567.25", L'.', &ok), 1234567.25, 1e-6); CHECK(ok);
    CHECK_NEAR(Num(L"1 234,5", L',', &ok), 1234.5, 1e-9); CHECK(ok);
    CHECK_NEAR(Num(L"1.5e-3", L'.', &ok), 0.0015, 1e-15); CHECK(ok);
    CHECK_NEAR(Num(L".5", L'.', &ok), 0.5, 1e-15); CHECK(ok);
    // Rejected numbers.
    Num(L"1,5", L'.', &ok); CHECK(!ok);
    Num(L"12,34.5", L'.', &ok); CHECK(!ok);
    Num(L"5;13", L',', &ok); CHECK(!ok);
    Num(L"abc", L'.', &ok); CHECK(!ok);
    Num(L"1e", L'.', &ok); CHECK(!ok);
    Num(L"", L'.', &ok); CHECK(!ok);

    core::NumberHints h;
    for (const wchar_t* c : {L"12,5", L"1,500", L"3,25"}) h.Observe(c);
    CHECK_EQ(h.Decimal(L';'), L',');
    CHECK_EQ(h.Decimal(L','), L'.');
    core::NumberHints g;
    for (const wchar_t* c : {L"1.500", L"2.000"}) g.Observe(c);
    // Ambiguous cells: the delimiter decides.
    CHECK_EQ(g.Decimal(L';'), L',');
    CHECK_EQ(g.Decimal(L'\t'), L'.');
    core::NumberHints m;
    m.Observe(L"1.234.567");
    CHECK_EQ(m.Decimal(L';'), L',');
    // Three decimals everywhere.
    core::NumberHints three;
    for (const wchar_t* c : {L"12,345", L"-98,250", L"230,125"}) three.Observe(c);
    CHECK_EQ(three.Decimal(L';'), L',');    // the semicolon decides
    CHECK_EQ(three.Decimal(L'\t'), L'.');   // nothing decides: the default
    three.Observe(L"0,532");                 // leading 0: not thousands
    CHECK_EQ(three.Decimal(L'\t'), L',');
    core::NumberHints wide;
    wide.Observe(L"1234,567");               // four digits before: not thousands
    CHECK_EQ(wide.Decimal(0), L',');
    core::NumberHints thousands;
    for (const wchar_t* c : {L"1,500", L"12,750", L"0.5"}) thousands.Observe(c);
    CHECK_EQ(thousands.Decimal(L'\t'), L'.');

    CHECK(core::IsMissing(L""));
    CHECK(core::IsMissing(L"NaN"));
    CHECK(core::IsMissing(L"#N/A"));
    CHECK(!core::IsMissing(L"0"));
}

void TestDates() {
    using core::DateOrder;
    core::DateTimeValue v;
    const double day = 86400.0 * static_cast<double>(core::DaysFromCivil(2026, 9, 25));
    CHECK(core::ParseDateTime(L"2026-09-25", DateOrder::DMY, &v));
    CHECK(v.has_date && !v.has_time);
    CHECK_NEAR(v.seconds, day, 1e-6);
    CHECK(core::ParseDateTime(L"2026-09-25T13:45:07.250Z", DateOrder::DMY, &v));
    CHECK_NEAR(v.seconds, day + 13 * 3600 + 45 * 60 + 7.25, 1e-6);
    CHECK(core::ParseDateTime(L"25.09.2026 13:45", DateOrder::DMY, &v));
    CHECK_NEAR(v.seconds, day + 13 * 3600 + 45 * 60, 1e-6);
    CHECK(core::ParseDateTime(L"09/25/2026 1:45:07 PM", DateOrder::MDY, &v));
    CHECK_NEAR(v.seconds, day + 13 * 3600 + 45 * 60 + 7, 1e-6);
    CHECK(core::ParseDateTime(L"12:00:00 AM", DateOrder::DMY, &v));
    CHECK_NEAR(v.seconds, 0.0, 1e-9);
    CHECK(core::ParseDateTime(L"13:45:07,5", DateOrder::DMY, &v));
    CHECK(!v.has_date && v.has_time);
    CHECK_NEAR(v.seconds, 13 * 3600 + 45 * 60 + 7.5, 1e-9);
    CHECK(core::ParseDateTime(L"2026-09-25 13:45:07+03:00", DateOrder::DMY, &v));
    CHECK(!core::ParseDateTime(L"12.5", DateOrder::DMY, &v));
    CHECK(!core::ParseDateTime(L"2026", DateOrder::DMY, &v));
    CHECK(!core::ParseDateTime(L"32.01.2026", DateOrder::DMY, &v));
    CHECK(!core::ParseDateTime(L"25:61", DateOrder::DMY, &v));

    core::DateHints h;
    h.Observe(L"09/25/2026");
    CHECK(h.Order() == DateOrder::MDY);
    core::DateHints g;
    g.Observe(L"05/06/2026");
    CHECK(g.Order() == DateOrder::DMY);

    CHECK_STR(core::FormatDateTime(day + 3723.5, true, 0.1), L"2026-09-25 01:02:03.5");
    CHECK_STR(core::FormatDateTime(3723.0, false, 1.0), L"01:02:03");
    CHECK_STR(core::FormatDateTime(86399.9996, false, 0.001), L"00:00:00.000");
}

// Parses inline test text.
bool FromText(const wchar_t* text, core::Table* t) {
    std::wstring err;
    return core::ParseTable(text, t, &err);
}

void TestInline() {
    core::Table t;
    // Semicolon delimiter with decimal commas.
    CHECK(FromText(L"a;b;c\n1,5;2,5;3,5\n1,6;2,6;3,6\n1,7;2,7;3,7\n", &t));
    CHECK(t.layout.kind == core::LayoutKind::Delimited);
    CHECK_EQ(t.layout.delimiter, L';');
    CHECK_EQ(t.layout.decimal, L',');
    CHECK_EQ(t.columns.size(), static_cast<size_t>(3));
    CHECK_NEAR(t.columns[1].values[2], 2.7, 1e-12);

    // Packed fixed-width columns.
    CHECK(FromText(L"00123ABC 4.5\n00124ABD 4.6\n00125ABE 4.7\n00126ABF 4.8\n", &t));
    CHECK(t.layout.kind == core::LayoutKind::FixedWidth);
    CHECK_EQ(t.columns.size(), static_cast<size_t>(3));
    CHECK(t.columns[0].kind == core::ColumnKind::Number);
    CHECK_NEAR(t.columns[0].values[3], 126.0, 0.0);

    // Nothing tabular.
    CHECK(!FromText(L"just one line of prose\nand another one\n", &t));

    // Unix time columns (seconds and milliseconds) become timestamps, not series.
    CHECK(FromText(L"Reg 0 epoch (s),Reg 0 value,Reg 2 epoch (s),Reg 2 value\n"
                   L"1790106126.846,-99.5,1790106126.846,12.5\n"
                   L"1790106126.946,248.25,1790106126.946,13.5\n"
                   L"1790106127.046,-218.0,1790106127.046,14.5\n"
                   L"1790106127.146,-10.0,1790106127.146,15.5\n", &t));
    CHECK_EQ(t.x_column, 0);
    if (t.columns.size() == 4) {
        CHECK(t.columns[0].kind == core::ColumnKind::DateTime);
        CHECK(t.columns[2].kind == core::ColumnKind::DateTime);
        CHECK_NEAR(t.columns[0].values[1] - t.columns[0].values[0], 0.1, 1e-6);
    }
    CHECK_EQ(t.series.size(), static_cast<size_t>(2));
    CHECK(FromText(L"time_ms;v\n1790106126846;1\n1790106126946;2\n1790106127046;3\n", &t));
    if (!t.columns.empty()) {
        CHECK(t.columns[0].kind == core::ColumnKind::DateTime);
        CHECK_NEAR(t.columns[0].values[2] - t.columns[0].values[0], 0.2, 1e-6);
    }

    // Header with no data rows.
    {
        std::wstring err;
        CHECK(!core::ParseTable(L"sep=,\nTimestamp,Epoch (s),Server,Value\n", &t, &err));
        CHECK_STR(err, L"The file has a header but no data rows.");
    }
}

void TestAxes() {
    using R = std::vector<std::pair<double, double>>;
    // A small-swing series goes right alone; constants stay left.
    std::vector<bool> r = core::ChooseAxes(
        R{{0, 17}, {-0.05, 0.06}, {118, 120}, {0, 17}, {0, 0}, {117.8, 117.8}});
    CHECK_EQ(r.size(), static_cast<size_t>(6));
    if (r.size() == 6) {
        CHECK(!r[0] && r[1] && !r[2] && !r[3] && !r[4] && !r[5]);
    }
    // Readable together already: nothing moves.
    r = core::ChooseAxes(R{{0, 100}, {10, 80}, {-20, 50}});
    CHECK(r == std::vector<bool>(3, false));
    // Two families far apart: the smaller family goes right.
    r = core::ChooseAxes(R{{12000, 26000}, {11000, 25000}, {0.95, 1.0}, {49.9, 50.1}});
    CHECK(r == (std::vector<bool>{false, false, true, true}));
    // One series, or only constants: nothing to split.
    CHECK(core::ChooseAxes(R{{0, 1}}) == std::vector<bool>(1, false));
    CHECK(core::ChooseAxes(R{{5, 5}, {100, 100}}) == std::vector<bool>(2, false));
}

std::wstring g_dir;

bool Load(const wchar_t* name, core::Table* t) {
    std::wstring err;
    const bool ok = core::LoadTableFile(g_dir + L"\\" + name, t, &err);
    if (!ok) std::printf("  %s: %s\n", core::ToUtf8(name).c_str(), core::ToUtf8(err).c_str());
    return ok;
}

void TestSamples() {
    core::Table t;

    if (!Load(L"tr_excel_semicolon.csv", &t)) {
        CHECK(false);
    } else {
        CHECK(t.encoding == core::Encoding::Ansi);
        CHECK_EQ(t.codepage, 1254u);
        CHECK(t.layout.kind == core::LayoutKind::Delimited);
        CHECK_EQ(t.layout.delimiter, L';');
        CHECK_EQ(t.layout.decimal, L',');
        CHECK_EQ(t.layout.preamble_lines, static_cast<size_t>(2));
        CHECK(t.layout.has_header);
        CHECK_EQ(t.layout.unit_rows, static_cast<size_t>(1));
        CHECK_EQ(t.rows, static_cast<size_t>(300));
        CHECK_EQ(t.x_column, 0);
        if (t.columns.size() == 5) {
            CHECK_STR(t.columns[0].name, L"Tarih Saat");
            CHECK(t.columns[1].merged_away);
            CHECK_STR(t.columns[2].name, L"Sıcaklık [°C]");
            CHECK_STR(t.columns[4].name, L"Güç [kW]");
            CHECK(t.columns[0].monotonic);   // across midnight
            CHECK_NEAR(t.columns[0].values.back() - t.columns[0].values.front(), 299.0, 1e-6);
            CHECK_NEAR(t.columns[2].values[1], 20.33, 1e-9);
        }
        CHECK_EQ(t.series.size(), static_cast<size_t>(3));
    }

    if (Load(L"iso_comma_bom.csv", &t)) {
        CHECK(t.encoding == core::Encoding::Utf8Bom);
        CHECK_EQ(t.layout.delimiter, L',');
        CHECK(t.layout.has_header);
        CHECK_EQ(t.rows, static_cast<size_t>(500));
        CHECK_EQ(t.x_column, 0);
        CHECK_EQ(t.series.size(), static_cast<size_t>(2));   // Status is text
        if (t.columns.size() == 4) {
            CHECK_STR(t.columns[1].name, L"Active Power (kW)");
            CHECK_EQ(t.columns[1].missing, static_cast<size_t>(1));
            CHECK(t.columns[3].kind == core::ColumnKind::Text);
            CHECK_NEAR(t.columns[0].values[1] - t.columns[0].values[0], 1.037, 1e-6);
        }
    } else { CHECK(false); }

    if (Load(L"tab_no_header.txt", &t)) {
        CHECK_EQ(t.layout.delimiter, L'\t');
        CHECK(!t.layout.has_header);
        CHECK_EQ(t.rows, static_cast<size_t>(200));
        CHECK_EQ(t.x_column, 0);
        CHECK_EQ(t.series.size(), static_cast<size_t>(2));
        if (t.columns.size() == 3) CHECK_STR(t.columns[1].name, L"Column 2");
    } else { CHECK(false); }

    if (Load(L"fixed_width_report.txt", &t)) {
        CHECK(t.layout.kind == core::LayoutKind::FixedWidth);
        CHECK(t.layout.has_header);
        CHECK_EQ(t.layout.preamble_lines, static_cast<size_t>(1));
        CHECK_EQ(t.rows, static_cast<size_t>(400));
        CHECK_EQ(t.x_column, 0);
        CHECK_EQ(t.series.size(), static_cast<size_t>(3));
        if (!t.columns.empty()) CHECK_STR(t.columns[0].name, L"Timestamp");
        for (int s : t.series) {
            const core::Column& c = t.columns[static_cast<size_t>(s)];
            if (c.name == L"Flow") CHECK_EQ(c.missing, static_cast<size_t>(5));
        }
        bool names = t.series.size() == 3 &&
                     t.columns[static_cast<size_t>(t.series[0])].name == L"TempInlet" &&
                     t.columns[static_cast<size_t>(t.series[1])].name == L"TempOutlet" &&
                     t.columns[static_cast<size_t>(t.series[2])].name == L"Flow";
        CHECK(names);
        if (t.x_column == 0) {
            CHECK_NEAR(t.columns[0].values[1] - t.columns[0].values[0], 10.0, 1e-6);
        }
    } else { CHECK(false); }

    if (Load(L"whitespace.dat", &t)) {
        CHECK(t.layout.kind == core::LayoutKind::Whitespace);
        CHECK(t.layout.has_header);
        CHECK_EQ(t.rows, static_cast<size_t>(150));
        CHECK_EQ(t.x_column, 0);
        CHECK_EQ(t.series.size(), static_cast<size_t>(2));
    } else { CHECK(false); }

    if (Load(L"utf16_tab.txt", &t)) {
        CHECK(t.encoding == core::Encoding::Utf16LE);
        CHECK_EQ(t.layout.delimiter, L'\t');
        CHECK_EQ(t.rows, static_cast<size_t>(120));
        CHECK_EQ(t.x_column, 0);
        if (t.columns.size() == 3) {
            CHECK(t.columns[0].kind == core::ColumnKind::Time);
            CHECK_STR(t.columns[1].name, L"Akım (A)");
        }
    } else { CHECK(false); }

    if (Load(L"pipe_repeated_header.txt", &t)) {
        CHECK_EQ(t.layout.delimiter, L'|');
        CHECK(t.layout.has_header);
        CHECK_EQ(t.rows, static_cast<size_t>(200));
        CHECK_EQ(t.skipped_rows, static_cast<size_t>(4));   // 3 repeated headers + footer
        CHECK_EQ(t.x_column, 0);
        CHECK_EQ(t.series.size(), static_cast<size_t>(2));
    } else { CHECK(false); }

    if (Load(L"misaligned_header_fixed.txt", &t)) {
        CHECK(t.layout.kind == core::LayoutKind::FixedWidth);
        CHECK(t.layout.has_header);
        CHECK_EQ(t.rows, static_cast<size_t>(1500));
        CHECK_EQ(t.x_column, 0);
        if (t.columns.size() == 6) {
            CHECK_STR(t.columns[0].name, L"Timestamp");
            CHECK_STR(t.columns[2].name, L"Epoch (s)");
            CHECK_STR(t.columns[3].name, L"Server 1 P PoC (MW) (Reg 0, FLOAT32)");
            CHECK_STR(t.columns[4].name, L"Server 1 Q PoC (MVAr) (Reg 2, FLOAT32)");
            CHECK_STR(t.columns[5].name, L"Server 1 F PoC (Hz) (Reg 6, FLOAT32)");
            CHECK(t.columns[2].axis_like);
            // Values wider than the sampled column are read whole.
            CHECK_EQ(t.columns[4].missing, static_cast<size_t>(0));
            CHECK_NEAR(t.columns[4].values[1200], -1.2345678901234567e-05 * (1 + 1200 % 7), 1e-20);
            CHECK_NEAR(t.columns[4].values[1201], -1.2345678901234567e-05 * (1 + 1201 % 7), 1e-20);
        } else {
            CHECK(false);
        }
        CHECK_EQ(t.series.size(), static_cast<size_t>(3));   // P, Q, F - not the epoch
    } else { CHECK(false); }

    if (Load(L"whitespace_named_header.txt", &t)) {
        CHECK(t.layout.kind == core::LayoutKind::Whitespace);
        CHECK(t.layout.has_header);
        CHECK_EQ(t.layout.preamble_lines, static_cast<size_t>(0));
        CHECK_EQ(t.rows, static_cast<size_t>(800));
        CHECK_EQ(t.x_column, 0);
        if (t.columns.size() == 6) {
            CHECK_STR(t.columns[0].name, L"Timestamp");
            CHECK_STR(t.columns[2].name, L"Epoch (s)");
            CHECK_STR(t.columns[3].name, L"Server 1 P PoC (MW) (Reg 0, FLOAT32)");
            CHECK_STR(t.columns[5].name, L"Server 1 F PoC (Hz) (Reg 6, FLOAT32)");
        } else {
            CHECK(false);
        }
        CHECK_EQ(t.series.size(), static_cast<size_t>(3));
    } else { CHECK(false); }

    if (Load(L"us_dates_quoted_thousands.csv", &t)) {
        CHECK_EQ(t.layout.delimiter, L',');
        CHECK_EQ(t.rows, static_cast<size_t>(100));
        CHECK_EQ(t.x_column, 0);
        if (t.columns.size() == 3) {
            CHECK(t.columns[0].kind == core::ColumnKind::DateTime);
            CHECK_NEAR(t.columns[0].values[0],
                       86400.0 * static_cast<double>(core::DaysFromCivil(2026, 9, 13)), 1e-6);
            // 12 PM is noon.
            CHECK_NEAR(t.columns[0].values[12] - t.columns[0].values[0], 12 * 3600.0, 1e-6);
            CHECK(t.columns[0].monotonic);
            CHECK_NEAR(t.columns[1].values[1], 1037.0, 0.0);
        }
    } else { CHECK(false); }
}

}  // namespace

// --bench FILE: times reading, decoding and parsing a file.
int Bench(const wchar_t* path) {
    LARGE_INTEGER f, t0, t1, t2, t3;
    QueryPerformanceFrequency(&f);
    auto ms = [&](LARGE_INTEGER a, LARGE_INTEGER b) {
        return 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart);
    };
    std::string bytes;
    std::wstring err;
    QueryPerformanceCounter(&t0);
    if (!core::ReadFileBytes(path, &bytes, &err)) return 1;
    QueryPerformanceCounter(&t1);
    core::DecodedText text;
    core::DecodeText(bytes, &text);
    QueryPerformanceCounter(&t2);
    core::Table t;
    if (!core::ParseTable(text.text, &t, &err)) return 1;
    QueryPerformanceCounter(&t3);
    std::printf("read %.0f ms, decode %.0f ms, parse %.0f ms, %zu rows\n", ms(t0, t1),
                ms(t1, t2), ms(t2, t3), t.rows);
    return 0;
}

// --dump FILE: prints the detected layout and columns.
int Dump(const wchar_t* path) {
    core::Table t;
    std::wstring err;
    if (!core::LoadTableFile(path, &t, &err)) {
        std::printf("error: %s\n", core::ToUtf8(err).c_str());
        return 1;
    }
    std::printf("%s | header %d | preamble %zu | rows %zu | skipped %zu | x %d\n",
                core::ToUtf8(core::DescribeLayout(t.layout)).c_str(), t.layout.has_header ? 1 : 0,
                t.layout.preamble_lines, t.rows, t.skipped_rows, t.x_column);
    for (size_t j = 0; j < t.columns.size(); ++j) {
        const core::Column& c = t.columns[j];
        std::printf("  %2zu kind %d missing %zu first %.17g  %s%s\n", j, static_cast<int>(c.kind),
                    c.missing, c.values.empty() ? 0.0 : c.values[0], core::ToUtf8(c.name).c_str(),
                    c.merged_away ? "  (merged)" : "");
    }
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc > 2 && std::wstring(argv[1]) == L"--dump") return Dump(argv[2]);
    if (argc > 2 && std::wstring(argv[1]) == L"--bench") return Bench(argv[2]);
    g_dir = argc > 1 ? argv[1] : L"tests\\samples";
    TestNumbers();
    TestDates();
    TestInline();
    TestAxes();
    TestSamples();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
