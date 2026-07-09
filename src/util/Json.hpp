#pragma once
#include <map>
#include <optional>
#include <string>

// Minimal JSON: parses ONE flat object of scalar values ({"text": "...",
// "alt": "...", "percentage": 42, "ok": true}) — exactly the shape Waybar
// custom modules emit. Nested objects/arrays are skipped (value recorded as
// ""). Numbers/bools/null are stringified. Full escape handling (\uXXXX,
// \n, \", \\, ...). Returns nullopt on malformed input.
namespace MiniJson {
    std::optional<std::map<std::string, std::string>> parseObject(const std::string& s);
    std::string escape(const std::string& s); // for building JSON output (hyprctl)
}
