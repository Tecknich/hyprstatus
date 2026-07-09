#include "Format.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
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

    std::string calendarGrid(time_t now) {
        struct tm tmNow{};
        localtime_r(&now, &tmNow);

        struct tm first = tmNow;
        first.tm_mday   = 1;
        first.tm_hour   = 12; // keep DST shifts from moving the date
        first.tm_min    = 0;
        first.tm_sec    = 0;
        first.tm_isdst  = -1;
        mktime(&first); // normalizes tm_wday

        const int MONCOL = (first.tm_wday + 6) % 7; // Monday-first column of day 1

        static const int DAYS[12]    = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        const int        YEAR        = tmNow.tm_year + 1900;
        int              daysInMonth = DAYS[tmNow.tm_mon];
        if (tmNow.tm_mon == 1 && YEAR % 4 == 0 && (YEAR % 100 != 0 || YEAR % 400 == 0))
            daysInMonth = 29;

        char title[64];
        strftime(title, sizeof(title), "%B %Y", &tmNow);

        std::string out = "   ";
        out += title;
        out += "\nMo Tu We Th Fr Sa Su";

        std::string row(21, ' ');
        size_t      rowOffset  = 0; // 1 after an in-row insert (today = 2-digit Monday)
        bool        rowHasDays = false;

        const auto FLUSHROW = [&]() {
            if (!rowHasDays)
                return;
            const size_t END = row.find_last_not_of(' ');
            out += '\n';
            out += row.substr(0, END == std::string::npos ? 0 : END + 1);
            row.assign(21, ' ');
            rowOffset  = 0;
            rowHasDays = false;
        };

        for (int day = 1; day <= daysInMonth; day++) {
            const int    CELL = (MONCOL + day - 1) % 7;
            const size_t POS  = static_cast<size_t>(CELL) * 3 + rowOffset;
            char         buf[4];
            std::snprintf(buf, sizeof(buf), "%2d", day);
            row[POS]     = buf[0];
            row[POS + 1] = buf[1];
            if (day == tmNow.tm_mday) {
                if (day < 10) { // " 8 " -> "[8]"
                    row[POS]     = '[';
                    row[POS + 2] = ']';
                } else if (POS > 0) { // steal the previous cell's trailing space
                    row[POS - 1] = '[';
                    row[POS + 2] = ']';
                } else { // 2-digit day in column 0: grow the row by one
                    row.insert(POS, "[");
                    row[POS + 3] = ']';
                    rowOffset    = 1;
                }
            }
            rowHasDays = true;
            if (CELL == 6)
                FLUSHROW();
        }
        FLUSHROW();
        return out; // no trailing newline
    }

    namespace {
        // one month rendered as fixed 8 lines x 20 cols (title, weekday header,
        // 6 week rows) so months tile cleanly into columns. todayMday > 0 marks
        // that day with [ ].
        std::array<std::string, 8> monthBlock(int year, int mon0, int todayMday) {
            struct tm first{};
            first.tm_year  = year - 1900;
            first.tm_mon   = mon0;
            first.tm_mday  = 1;
            first.tm_hour  = 12;
            first.tm_isdst = -1;
            mktime(&first);

            const int MONCOL = (first.tm_wday + 6) % 7;

            static const int DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            int              dim      = DAYS[mon0];
            if (mon0 == 1 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
                dim = 29;

            char name[16];
            strftime(name, sizeof(name), "%B", &first);

            std::array<std::string, 8> lines;
            std::string                title(name);
            const int                  padL = (int)std::max<long>(0, (20 - (long)title.size()) / 2);
            lines[0] = std::string(padL, ' ') + title;
            lines[0].resize(20, ' ');
            lines[1] = "Mo Tu We Th Fr Sa Su";
            for (int i = 2; i < 8; i++)
                lines[i] = std::string(20, ' ');

            for (int day = 1; day <= dim; day++) {
                const int    cell = (MONCOL + day - 1) % 7;
                const int    week = (MONCOL + day - 1) / 7;
                std::string& row  = lines[2 + week];
                const size_t pos  = (size_t)cell * 3;
                char         buf[4];
                std::snprintf(buf, sizeof(buf), "%2d", day);
                row[pos]     = buf[0];
                row[pos + 1] = buf[1];
                if (day == todayMday) { // steal a neighbouring space for the brackets
                    if (day < 10) {
                        row[pos]     = '[';
                        row[pos + 2] = ']';
                    } else if (pos > 0) {
                        row[pos - 1] = '[';
                        row[pos + 2] = ']';
                    }
                }
            }
            return lines;
        }
    }

    std::string calendarYear(time_t now) {
        struct tm tmNow{};
        localtime_r(&now, &tmNow);
        const int YEAR = tmNow.tm_year + 1900;

        char title[16];
        std::snprintf(title, sizeof(title), "%d", YEAR);

        std::string out = "              " + std::string(title); // roughly centered over 3 columns

        for (int block = 0; block < 4; block++) { // 4 rows of 3 months
            std::array<std::array<std::string, 8>, 3> cols;
            for (int c = 0; c < 3; c++) {
                const int MON = block * 3 + c;
                cols[c]       = monthBlock(YEAR, MON, MON == tmNow.tm_mon ? tmNow.tm_mday : -1);
            }
            out += '\n';
            for (int line = 0; line < 8; line++) {
                out += '\n';
                out += cols[0][line] + "   " + cols[1][line] + "   " + cols[2][line];
            }
        }
        return out;
    }
}
