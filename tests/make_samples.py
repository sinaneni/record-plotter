"""Writes the sample record files in tests/samples, one layout case per file.

Run:  python tests/make_samples.py
"""

import math
from pathlib import Path

OUT = Path(__file__).resolve().parent / "samples"


def write(name, text, encoding="utf-8", newline="\r\n"):
    data = text.replace("\n", newline).encode(encoding)
    (OUT / name).write_bytes(data)


def rows(n, t0=0):
    for i in range(n):
        t = t0 + i
        yield i, t, 20 + 5 * math.sin(i / 15), 230 + 3 * math.cos(i / 7), 50 + (i % 40) * 0.25


def clock(sec):
    return f"{sec // 3600 % 24:02d}:{sec // 60 % 60:02d}:{sec % 60:02d}"


def main():
    OUT.mkdir(exist_ok=True)

    # 1. Turkish Excel export: ';', decimal comma, preamble, units row, Windows-1254.
    lines = ["Cihaz: PLC-07 Saha Kaydı", "Dışa aktarım: 26.09.2026 08:15", "",
             "Tarih;Saat;Sıcaklık;Gerilim;Güç",
             ";;[°C];[V];[kW]"]
    for i, t, a, b, c in rows(300, t0=23 * 3600 + 58 * 60):
        day = 25 if t < 86400 else 26
        comma = lambda v, d: f"{v:.{d}f}".replace(".", ",")
        lines.append(f"{day:02d}.09.2026;{clock(t)};{comma(a, 2)};{comma(b, 1)};{comma(c, 3)}")
    write("tr_excel_semicolon.csv", "\n".join(lines) + "\n", encoding="cp1254")

    # 2. ISO timestamps, comma delimiter, quoted header, a missing value, UTF-8 with BOM.
    lines = ['"timestamp","Active Power (kW)","Reactive Power (kvar)","Status"']
    for i, t, a, b, c in rows(500):
        p = "" if i == 42 else f"{a * 10:.3f}"
        lines.append(f"2026-09-25T10:{i // 60 % 60:02d}:{i % 60:02d}.{(i * 37) % 1000:03d},{p},{b - 230:.3f},OK")
    write("iso_comma_bom.csv", "\n".join(lines) + "\n", encoding="utf-8-sig", newline="\n")

    # 3. Tab separated, no header, a leading counter.
    lines = [f"{i}\t{a:.4f}\t{b:.4f}" for i, t, a, b, c in rows(200)]
    write("tab_no_header.txt", "\n".join(lines) + "\n")

    # 4. Fixed width with wide header words, an empty cell and a text column.
    lines = ["REPORT  UNIT 3        GENERATED 2026-09-25", "",
             "Timestamp            TempInlet  TempOutlet  Flow  Alarm text"]
    for i, t, a, b, c in rows(400):
        flow = "" if i % 97 == 5 else f"{c:6.2f}"
        alarm = "HIGH TEMP" if a > 24.5 else "none"
        lines.append(f"2026-09-25 {clock(36000 + i * 10)}  {a:8.3f}  {b:10.2f}  {flow:>6}  {alarm}")
    write("fixed_width_report.txt", "\n".join(lines) + "\n")

    # 5. Whitespace separated, not aligned.
    lines = ["t x y"]
    for i, t, a, b, c in rows(150):
        lines.append(f"{i * 0.01:g} {a:g} {b:g}")
    write("whitespace.dat", "\n".join(lines) + "\n", newline="\n")

    # 6. UTF-16 LE with a BOM, tab separated.
    lines = ["Zaman\tAkım (A)\tFrekans (Hz)"]
    for i, t, a, b, c in rows(120):
        lines.append(f"{clock(3600 * 14 + i)}\t{a:.2f}\t{50 + (b - 230) / 100:.3f}")
    write("utf16_tab.txt", "\n".join(lines) + "\n", encoding="utf-16")

    # 7. Pipe delimited, a header repeated every 50 rows, and a footer.
    lines = []
    for i, t, a, b, c in rows(200):
        if i % 50 == 0:
            lines.append("Sample|Voltage|Current")
        lines.append(f"{i}|{b:.2f}|{a / 10:.3f}")
    lines.append("Total rows: 200")
    write("pipe_repeated_header.txt", "\n".join(lines) + "\n")

    # 8. US dates with AM/PM, quoted thousands separators.
    lines = ["Date,Energy (Wh),Temp"]
    for i, t, a, b, c in rows(100):
        h24 = i % 24
        h12 = h24 % 12 or 12
        lines.append(f'09/{13 + i // 24:02d}/2026 {h12}:00:00 {"AM" if h24 < 12 else "PM"},'
                     f'"{1000 + i * 37:,}",{a:.1f}')
    write("us_dates_quoted_thousands.csv", "\n".join(lines) + "\n")

    # 9. Fixed-width data under a misaligned header; values widen later in the file.
    names = ["Timestamp", "Epoch (s)", "Server 1 P PoC (MW) (Reg 0, FLOAT32)",
             "Server 1 Q PoC (MVAr) (Reg 2, FLOAT32)", "Server 1 F PoC (Hz) (Reg 6, FLOAT32)"]
    lines = ["  ".join(names)]
    epoch0 = 1789111870.219
    for i in range(1500):
        e = epoch0 + i * 0.05
        ms = round((e % 1) * 1000) % 1000
        sec = int(e) % 86400
        p = f"{12 + math.sin(i / 50):.6f}"
        q = f"{0.003 * math.cos(i / 30):.6f}" if i < 1200 else repr(-1.2345678901234567e-05 * (1 + i % 7))
        lines.append(f"2026-09-11 {clock(sec)}.{ms:03d}  {e:.3f}  {p:<36}  {q:<38}  {50 + (i % 5) / 100:.2f}")
    write("misaligned_header_fixed.txt", "\n".join(lines) + "\n", encoding="utf-8-sig")

    # 10. Overflowing values: whitespace separated under a header with spaced names.
    lines = ["  ".join(names)]
    for i in range(800):
        e = epoch0 + i * 0.05
        ms = round((e % 1) * 1000) % 1000
        p = repr(12 + math.sin(i / 50))                       # 17 or 18 characters
        q = repr(0.003 * math.cos(i / 30) * (10 ** -(i % 4)))  # 5 to 22 characters
        lines.append(f"2026-09-11 {clock(int(e) % 86400)}.{ms:03d}  {e:.3f}  {p}  {q}  {50 + (i % 5) / 100:.2f}")
    write("whitespace_named_header.txt", "\n".join(lines) + "\n", encoding="utf-8-sig")


if __name__ == "__main__":
    main()
