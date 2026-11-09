#pragma once
#include <map>
#include <string>

// Per-module options, set from the Hyprland Lua config (0.57 has no other
// config provider):
//
//   hl.plugin.hyprstatus.module("nowplaying")              -- declare a custom module
//   hl.plugin.hyprstatus.set("nowplaying", "interval", "2")
//   hl.plugin.hyprstatus.set("clock", "format", "%a, %b %d   %I:%M %p")
//
// Arguments are plain strings, so commas, quotes and real newlines in a value
// need no escaping. Option keys follow Waybar naming (format, interval,
// on-click, format-icons.<alt>, states.warning, color.<class>, ...).
struct SModuleConfig {
    std::string                        name;
    std::map<std::string, std::string> opts;
};

namespace ModuleConfigStore {
    // register hl.plugin.hyprstatus.module / .set. PLUGIN_INIT only.
    void registerKeywords();

    // Reload bracketing. A Lua syntax error aborts the reload before any of the
    // config runs, yet Hyprland keeps its old state and still emits
    // config.reloaded -- clearing on preReload alone would then rebuild the bar
    // with no per-module options at all. So: beginReload() parks the current
    // options (config.preReload); module()/set() call noteLuaCall(); and
    // endReload(verified) restores the parked set when verification failed AND
    // no module()/set() ran this time (config.reloaded, before the rebuild).
    void beginReload();
    void noteLuaCall();
    void endReload(bool verificationPassed);

    const std::map<std::string, SModuleConfig>& all();
    SModuleConfig                               get(const std::string& name); // empty cfg if unknown
    bool                                        isDeclaredCustom(const std::string& name);
}
