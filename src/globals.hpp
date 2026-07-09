#pragma once
#define WLR_USE_UNSTABLE
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/values/ConfigValues.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>

class CBarManager;

// Global bar options, registered as typed plugin:hyprstatus:* config values.
// The SPs must stay alive for the plugin lifetime (the compositor only keeps
// weak refs).
struct SGlobalConfig {
    SP<Config::Values::CBoolValue>   enabled;
    SP<Config::Values::CStringValue> position; // "top" | "bottom"
    SP<Config::Values::CIntValue>    height;
    SP<Config::Values::CIntValue>    margin;      // outer gap: top(/bottom) + left + right, logical px
    SP<Config::Values::CIntValue>    spacing;     // gap between modules
    SP<Config::Values::CIntValue>    padding;     // default horizontal padding inside a module
    SP<Config::Values::CIntValue>    rounding;
    SP<Config::Values::CIntValue>    borderSize;
    SP<Config::Values::CBoolValue>   blur;
    SP<Config::Values::CBoolValue>   hideOnFullscreen;
    SP<Config::Values::CBoolValue>   tooltips;
    SP<Config::Values::CIntValue>    tooltipDelayMs;
    SP<Config::Values::CStringValue> fontFamily;
    SP<Config::Values::CIntValue>    fontSize;    // logical pt-ish px, scaled per monitor
    SP<Config::Values::CStringValue> modulesLeft, modulesCenter, modulesRight; // space-separated names
    SP<Config::Values::CStringValue> monitors;    // space-separated monitor names; empty = all
    SP<Config::Values::CStringValue> iconTheme;   // tray icon theme; empty = hicolor-first
    SP<Config::Values::CIntValue>    trayIconSize;

    SP<Config::Values::CColorValue>  colBackground, colForeground, colForegroundBright,
                                     colBorder, colAccent, colAccentDim,
                                     colOk, colWarn, colErr,
                                     colTooltipBg, colTooltipFg;
};

inline HANDLE          PHANDLE = nullptr;
inline SGlobalConfig   g_cfg;
inline UP<CBarManager> g_barManager;

inline CHyprColor cfgColor(const SP<Config::Values::CColorValue>& v) {
    return CHyprColor{(uint64_t)v->value()};
}
