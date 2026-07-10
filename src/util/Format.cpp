#include "Format.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string_view>

namespace {
    size_t utf8Len(const std::string& s) {
        size_t n = 0;
        for (const char C : s)
            if ((static_cast<unsigned char>(C) & 0xC0) != 0x80)
                n++;
        return n;
    }
}

namespace Fmt {
    std::string replaceTokens(const std::string& fmt, const std::map<std::string, std::string>& tokens) {
        std::string out;
        out.reserve(fmt.size());
        size_t i = 0;
        while (i < fmt.size()) {
            if (fmt[i] != '{') {
                out += fmt[i++];
                continue;
            }

            const size_t CLOSE = fmt.find('}', i + 1);
            bool         ok    = CLOSE != std::string::npos;
            std::string  name;
            char         align = 0;
            size_t       width = 0;
            if (ok) {
                const std::string INNER = fmt.substr(i + 1, CLOSE - i - 1);
                const size_t      COLON = INNER.find(':');
                if (COLON == std::string::npos)
                    name = INNER;
                else {
                    name = INNER.substr(0, COLON);
                    const std::string SPEC = INNER.substr(COLON + 1);
                    if (SPEC.size() < 2 || (SPEC[0] != '>' && SPEC[0] != '<'))
                        ok = false;
                    else {
                        align = SPEC[0];
                        for (size_t k = 1; ok && k < SPEC.size(); k++) {
                            if (SPEC[k] < '0' || SPEC[k] > '9')
                                ok = false;
                            else
                                width = std::min<size_t>(width * 10 + (SPEC[k] - '0'), 4096);
                        }
                    }
                }
                // waybar token names are [A-Za-z0-9_-]; anything else (spaces,
                // '{', '%', ...) means this '{' is literal text
                for (size_t k = 0; ok && k < name.size(); k++) {
                    const char N = name[k];
                    if (!(N >= 'a' && N <= 'z') && !(N >= 'A' && N <= 'Z') && !(N >= '0' && N <= '9') && N != '_' && N != '-')
                        ok = false;
                }
            }

            if (!ok) { // '{' not forming a valid token: verbatim, rescan from next char
                out += '{';
                i++;
                continue;
            }

            std::string val;
            if (name.empty()) {
                if (const auto IT = tokens.find(""); IT != tokens.end())
                    val = IT->second;
                else if (const auto IT2 = tokens.find("text"); IT2 != tokens.end())
                    val = IT2->second;
            } else if (const auto IT = tokens.find(name); IT != tokens.end())
                val = IT->second;

            if (width > 0) {
                const size_t LEN = utf8Len(val);
                if (LEN < width) {
                    const std::string PAD(width - LEN, ' ');
                    val = align == '>' ? PAD + val : val + PAD;
                }
            }
            out += val;
            i = CLOSE + 1;
        }
        return out;
    }

    std::string humanBits(double bitsPerSec) {
        static const char* UNITS[] = {"b/s", "Kb/s", "Mb/s", "Gb/s"};
        double             v = bitsPerSec < 0 ? 0 : bitsPerSec;
        size_t             u = 0;
        while (v >= 1000.0 && u < 3) {
            v /= 1000.0;
            u++;
        }
        char buf[32];
        if (v < 10.0)
            std::snprintf(buf, sizeof(buf), "%.1f%s", v, UNITS[u]);
        else
            std::snprintf(buf, sizeof(buf), "%.0f%s", v, UNITS[u]);
        return buf;
    }

    std::string humanBytes(double bytes) {
        static const char* UNITS[] = {"B", "KiB", "MiB", "GiB"};
        double             v = bytes < 0 ? 0 : bytes;
        size_t             u = 0;
        while (v >= 1024.0 && u < 3) {
            v /= 1024.0;
            u++;
        }
        char buf[32];
        if (v < 10.0)
            std::snprintf(buf, sizeof(buf), "%.1f %s", v, UNITS[u]);
        else
            std::snprintf(buf, sizeof(buf), "%.0f %s", v, UNITS[u]);
        return buf;
    }

    std::string strftimeFmt(const std::string& fmt, time_t t) {
        std::string f = fmt;
        if (f.size() >= 3 && f.starts_with("{:") && f.back() == '}')
            f = f.substr(2, f.size() - 3);
        if (f.empty())
            return "";

        struct tm tmv{};
        localtime_r(&t, &tmv);

        std::string buf;
        for (size_t cap = 64; cap <= 1024; cap *= 2) {
            buf.resize(cap);
            const size_t N = strftime(buf.data(), cap, f.c_str(), &tmv);
            if (N > 0) {
                buf.resize(N);
                return buf;
            }
        }
        return "";
    }

    std::string trim(const std::string& s) {
        const size_t A = s.find_first_not_of(" \t\n\r\f\v");
        if (A == std::string::npos)
            return "";
        const size_t B = s.find_last_not_of(" \t\n\r\f\v");
        return s.substr(A, B - A + 1);
    }

    std::string truncate(const std::string& s, size_t maxLen) {
        if (maxLen == 0)
            return s;
        size_t cps = 0;
        for (size_t i = 0; i < s.size(); i++) {
            if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) { // code point start
                if (cps == maxLen)
                    return s.substr(0, i) + "…";
                cps++;
            }
        }
        return s;
    }

    std::string stripPango(const std::string& s) {
        static constexpr std::pair<std::string_view, char> ENTITIES[] = {
            {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};

        std::string out;
        out.reserve(s.size());
        size_t i = 0;
        while (i < s.size()) {
            const char C = s[i];
            if (C == '<') {
                const size_t CLOSE = s.find('>', i + 1);
                if (CLOSE == std::string::npos) { // unmatched '<' passes through
                    out += '<';
                    i++;
                } else
                    i = CLOSE + 1;
            } else if (C == '&') {
                bool matched = false;
                for (const auto& [ENT, CH] : ENTITIES) {
                    if (s.compare(i, ENT.size(), ENT) == 0) {
                        out += CH;
                        i += ENT.size();
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    out += '&';
                    i++;
                }
            } else {
                out += C;
                i++;
            }
        }
        return out;
    }

    std::vector<std::string> split(const std::string& s, char sep) {
        std::vector<std::string> out;
        size_t                   start = 0;
        while (start <= s.size()) {
            size_t end = s.find(sep, start);
            if (end == std::string::npos)
                end = s.size();
            if (end > start) // skip empties
                out.emplace_back(s.substr(start, end - start));
            start = end + 1;
        }
        return out;
    }

    std::string escapeMarkup(const std::string& s) {
        std::string o;
        o.reserve(s.size());
        for (const char C : s) {
            switch (C) {
                case '&': o += "&amp;"; break;
                case '<': o += "&lt;"; break;
                case '>': o += "&gt;"; break;
                case '"': o += "&quot;"; break;
                case '\'': o += "&#39;"; break;
                default: o += C; break;
            }
        }
        return o;
    }

    namespace {
        // Escape the three Pango-markup metacharacters. Our calendar text is
        // digits/spaces/letters, but strftime month names come from the locale,
        // so the title is escaped defensively.
        std::string esc(const std::string& s) {
            std::string o;
            o.reserve(s.size());
            for (const char C : s) {
                switch (C) {
                    case '&': o += "&amp;"; break;
                    case '<': o += "&lt;"; break;
                    case '>': o += "&gt;"; break;
                    default: o += C; break;
                }
            }
            return o;
        }

        // ISO-8601 week number of a date (year, 0-based month, 1-based day; the
        // day may be out of range — mktime normalizes into an adjacent month).
        // strftime %V is the ISO-8601 week: weeks run Monday..Sunday and week 1
        // is the week containing the year's first Thursday. mktime sets tm_wday
        // and tm_yday, which %V needs.
        int isoWeek(int year, int mon0, int mday) {
            struct tm t{};
            t.tm_year  = year - 1900;
            t.tm_mon   = mon0;
            t.tm_mday  = mday;
            t.tm_hour  = 12; // avoid DST edges shifting the date
            t.tm_isdst = -1;
            mktime(&t);
            // strftime returns 0 and leaves buf indeterminate on overflow; don't
            // atoi() an indeterminate buffer.
            char buf[8] = {};
            if (strftime(buf, sizeof(buf), "%V", &t) == 0)
                return 0;
            return std::atoi(buf);
        }

        static const int MONTH_DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

        int daysInMonth(int year, int mon0) {
            int d = MONTH_DAYS[mon0];
            if (mon0 == 1 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
                d = 29;
            return d;
        }

        // Sunday-first column (0=Su .. 6=Sa) of the 1st of month, == tm_wday.
        int firstDayColumn(int year, int mon0) {
            struct tm first{};
            first.tm_year  = year - 1900;
            first.tm_mon   = mon0;
            first.tm_mday  = 1;
            first.tm_hour  = 12;
            first.tm_isdst = -1;
            mktime(&first);
            return first.tm_wday;
        }

        // One markup week-row line: 7 day cells (2 chars, single-space separated
        // = 20 visible cols) + "  Wnn" (5 cols) = 25 visible cols exactly. Spans
        // add no visible width, so monospace columns stay aligned. `today` (-1 to
        // disable) gets an accent-background highlight. `weekMday` is the 1-based
        // day-of-month of that row's Thursday (may be <1 or >dim; normalized).
        std::string weekRow(int year, int mon0, int r, int sunCol, int dim, int today, const std::string& accentHex, const std::string& dimHex) {
            std::string line;
            for (int c = 0; c < 7; c++) {
                if (c)
                    line += ' ';
                const int DAYNUM = r * 7 + c - sunCol + 1;
                if (DAYNUM < 1 || DAYNUM > dim) {
                    line += "  ";
                    continue;
                }
                char cell[4];
                std::snprintf(cell, sizeof(cell), "%2d", DAYNUM);
                if (DAYNUM == today) {
                    line += "<span background='" + accentHex + "' foreground='#101010'><b>";
                    line += cell; // still 2 visible chars; span carries no width
                    line += "</b></span>";
                } else
                    line += cell;
            }
            const int  THU_MDAY = r * 7 - sunCol + 5; // Thursday is Sunday-first column 4
            const int  WK       = isoWeek(year, mon0, THU_MDAY);
            char       wbuf[8];
            std::snprintf(wbuf, sizeof(wbuf), "W%02d", WK);
            line += "  <span foreground='" + dimHex + "'>";
            line += wbuf;
            line += "</span>";
            return line;
        }
    }

    std::string calendarGrid(time_t now, const std::string& accentHex, const std::string& fgHex, const std::string& dimHex) {
        struct tm tmNow{};
        localtime_r(&now, &tmNow);

        const int YEAR   = tmNow.tm_year + 1900;
        const int MON0   = tmNow.tm_mon;
        const int TODAY  = tmNow.tm_mday;
        const int SUNCOL = firstDayColumn(YEAR, MON0);
        const int DIM    = daysInMonth(YEAR, MON0);

        // strftime returns 0 (and leaves the buffer INDETERMINATE) when the
        // formatted result doesn't fit; reading it as a C-string then is an OOB
        // read. Zero-init and fall back to a numeric title on 0.
        char title[64] = {};
        if (strftime(title, sizeof(title), "%B %Y", &tmNow) == 0)
            std::snprintf(title, sizeof(title), "%04d-%02d", YEAR, MON0 + 1);

        // whole grid wrapped in one FG span; title/weeknum/today override it.
        std::string out = "<span foreground='" + fgHex + "'>";
        out += "<span foreground='" + accentHex + "'><b>" + esc(title) + "</b></span>\n";
        out += "Su Mo Tu We Th Fr Sa\n";

        const int NUMROWS = (SUNCOL + DIM + 6) / 7;
        for (int r = 0; r < NUMROWS; r++) {
            out += weekRow(YEAR, MON0, r, SUNCOL, DIM, TODAY, accentHex, dimHex);
            if (r + 1 < NUMROWS)
                out += '\n';
        }
        out += "</span>";
        return out;
    }

    namespace {
        // One month rendered as a fixed 8 markup lines (title, weekday header, up
        // to 6 week rows, padded), each exactly 25 visible cols wide so three
        // months tile side by side with aligned columns. Each content line is
        // wrapped in its own FG span; blank pad lines are 25 spaces.
        std::array<std::string, 8> monthBlockMarkup(int year, int mon0, int todayMday, const std::string& accentHex, const std::string& fgHex,
                                                    const std::string& dimHex) {
            constexpr int BLOCKW = 25; // 20 day cols + 2 gap + 3 "Wnn"
            const int     SUNCOL = firstDayColumn(year, mon0);
            const int     DIM    = daysInMonth(year, mon0);

            // strftime returns 0 and leaves name indeterminate on overflow;
            // zero-init and fall back to a numeric month so it stays a valid,
            // NUL-terminated C-string before the std::string(name) read below.
            char name[32] = {};
            {
                struct tm t{};
                t.tm_year  = year - 1900;
                t.tm_mon   = mon0;
                t.tm_mday  = 1;
                t.tm_hour  = 12;
                t.tm_isdst = -1;
                mktime(&t);
                if (strftime(name, sizeof(name), "%B", &t) == 0)
                    std::snprintf(name, sizeof(name), "%d", mon0 + 1);
            }

            std::array<std::string, 8> lines;

            std::string title(name);
            if ((int)title.size() > BLOCKW)
                title = title.substr(0, BLOCKW);
            const int PADL = (BLOCKW - (int)title.size()) / 2;
            const int PADR = BLOCKW - (int)title.size() - PADL;
            lines[0] = std::string(PADL, ' ') + "<span foreground='" + accentHex + "'><b>" + esc(title) + "</b></span>" + std::string(PADR, ' ');

            lines[1] = "<span foreground='" + fgHex + "'>Su Mo Tu We Th Fr Sa     </span>"; // 20 + 5 = 25

            const int NUMROWS = (SUNCOL + DIM + 6) / 7;
            for (int r = 0; r < 6; r++) {
                if (r >= NUMROWS) {
                    lines[2 + r] = std::string(BLOCKW, ' ');
                    continue;
                }
                lines[2 + r] = "<span foreground='" + fgHex + "'>" + weekRow(year, mon0, r, SUNCOL, DIM, todayMday, accentHex, dimHex) + "</span>";
            }
            return lines;
        }
    }

    std::string calendarYear(time_t now, const std::string& accentHex, const std::string& fgHex, const std::string& dimHex) {
        struct tm tmNow{};
        localtime_r(&now, &tmNow);
        const int YEAR = tmNow.tm_year + 1900;

        constexpr int TOTALW = 3 * 25 + 2 * 3; // 3 blocks + two 3-space gaps = 81
        char          ybuf[16];
        std::snprintf(ybuf, sizeof(ybuf), "%d", YEAR);
        const std::string YTITLE(ybuf);
        const int         PADL = std::max(0, (TOTALW - (int)YTITLE.size()) / 2);

        std::string out;
        out += std::string(PADL, ' ') + "<span foreground='" + accentHex + "'><b>" + YTITLE + "</b></span>\n\n";

        for (int block = 0; block < 4; block++) { // 4 rows of 3 months
            std::array<std::array<std::string, 8>, 3> cols;
            for (int c = 0; c < 3; c++) {
                const int MON = block * 3 + c;
                cols[c]       = monthBlockMarkup(YEAR, MON, MON == tmNow.tm_mon ? tmNow.tm_mday : -1, accentHex, fgHex, dimHex);
            }
            for (int line = 0; line < 8; line++) {
                out += cols[0][line] + "   " + cols[1][line] + "   " + cols[2][line];
                out += '\n';
            }
            if (block + 1 < 4)
                out += '\n'; // blank line between month-block rows
        }
        return out;
    }
}
