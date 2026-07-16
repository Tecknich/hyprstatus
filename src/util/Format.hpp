#pragma once
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Waybar-flavored formatting helpers.
namespace Fmt {
    // ---- locale-free /proc & /sys readers -----------------------------------
    // C++ streams (<fstream>/<sstream>) bind libstdc++'s locale-facet
    // STB_GNU_UNIQUE symbols; the dynamic linker marks any DSO that binds a
    // unique symbol NODELETE, which silently breaks dlclose() and therefore
    // `hyprctl plugin unload` (stale plugin stays resident; reload re-inits the
    // dead copy). These helpers replace every stream use in the plugin — do NOT
    // reintroduce <fstream>/<sstream>/<iostream> anywhere. See issue #12.

    // whole (small) file via stdio; empty string on any error
    std::string readFile(const std::string& path);
    // first line, trailing \n / \r stripped; nullopt only if the file cannot
    // be opened or read (a readable-but-empty file yields "")
    std::optional<std::string> readLine(const std::string& path);
    // split on runs of whitespace, no empty tokens (the `iss >> tok` equivalent)
    std::vector<std::string> tokens(const std::string& s);
    // locale-independent numeric parses of a trimmed token (std::from_chars);
    // nullopt when no number can be parsed at the start of the string
    std::optional<long long>          toLL(std::string_view s, int base = 10);
    std::optional<unsigned long long> toULL(std::string_view s, int base = 10);
    std::optional<double>             toDouble(std::string_view s);

    // Replaces {key} and {key:>N} / {key:<N} (right/left pad to width N) with
    // tokens.at(key); unknown tokens become "". "{}" is an alias for
    // tokens.at("") if present, else tokens.at("text").
    std::string replaceTokens(const std::string& fmt, const std::map<std::string, std::string>& tokens);

    // "3.4Mb/s" style SI formatting of a bits-per-second rate
    std::string humanBits(double bitsPerSec);
    // "1.2 GiB" style
    std::string humanBytes(double bytes);

    // strftime with std::string; handles waybar's "{:%a %H:%M}" wrapper too
    // (strips a leading "{:" and trailing "}" if present)
    std::string strftimeFmt(const std::string& fmt, time_t t);

    std::string trim(const std::string& s);
    // UTF-8-safe truncation to maxLen code points, appends "…" when truncated;
    // maxLen 0 = no limit
    std::string truncate(const std::string& s, size_t maxLen);
    // strip pango markup tags (<span ...>, <b>, ...) for plain-text rendering
    std::string stripPango(const std::string& s);

    // Escape Pango/XML markup metacharacters so external text can be embedded in
    // a markup string without injecting spans/entities: & < > and also " and '
    // for attribute-value safety. Any module that emits PANGO MARKUP MUST route
    // untrusted/external text (window titles, tags, filenames, ...) through this.
    std::string escapeMarkup(const std::string& s);

    std::vector<std::string> split(const std::string& s, char sep);

    // Month calendar grid for the clock tooltip, emitted as PANGO MARKUP (the
    // caller must render it via the markup path, not plain text). Waybar look:
    // Sunday-first weekday header, days right-aligned width 2, an ISO-8601 week
    // number "Wnn" column at the end of each week, an accent+bold month/year
    // title, and today drawn with an accent background span. Colors are supplied
    // as "#RRGGBB" hex strings (accent = title + today highlight, fg = header +
    // days, dim = week numbers). e.g.
    //   July 2026
    //   Su Mo Tu We Th Fr Sa
    //             1  2  3  4  W26
    //    5  6  7  8 [9]10 11  W27
    std::string calendarGrid(time_t now, const std::string& accentHex, const std::string& fgHex, const std::string& dimHex);

    // Full-year markup grid: 12 mini month calendars, 3 per row across 4 rows,
    // each with its own title, weekday header and ISO week-number column, today
    // highlighted. Same color convention as calendarGrid.
    std::string calendarYear(time_t now, const std::string& accentHex, const std::string& fgHex, const std::string& dimHex);
}
