#pragma once
#include <map>
#include <string>
#include <vector>

// Waybar-flavored formatting helpers.
namespace Fmt {
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

    std::vector<std::string> split(const std::string& s, char sep);

    // month calendar grid for the clock tooltip, e.g.
    //      July 2026
    //  Mo Tu We Th Fr Sa Su
    //         1  2  3  4  5
    //  ...        [today marked]
    std::string calendarGrid(time_t now);
}
