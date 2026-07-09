#pragma once
#include <map>
#include <string>

// Per-module options accumulated from repeatable config keywords:
//
//   hyprstatus-module = nowplaying                 # declare a custom module
//   hyprstatus-set = nowplaying, interval, 2
//   hyprstatus-set = clock, format, %a, %b %d   %I:%M %p
//
// hyprstatus-set splits ONLY the first two commas; the remainder is the raw
// value (commas and leading/trailing spaces preserved except around the first
// two separators). Option keys follow Waybar naming (format, interval,
// on-click, format-icons.<alt>, states.warning, color.<class>, ...).
//
// On the Lua config backend the equivalents are
//   hl.plugin.hyprstatus.module("nowplaying")
//   hl.plugin.hyprstatus.set("clock", "format", "%a %b %d")
struct SModuleConfig {
    std::string                        name;
    std::map<std::string, std::string> opts;
};

namespace ModuleConfigStore {
    // register hyprstatus-module / hyprstatus-set on the active config backend.
    // MUST be called from PLUGIN_INIT only.
    void registerKeywords();

    void clear(); // config.preReload

    const std::map<std::string, SModuleConfig>& all();
    SModuleConfig                               get(const std::string& name); // empty cfg if unknown
    bool                                        isDeclaredCustom(const std::string& name);
}
