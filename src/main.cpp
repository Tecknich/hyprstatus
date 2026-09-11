#define WLR_USE_UNSTABLE
#include "globals.hpp"

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/event/EventBus.hpp>

#include "config/ModuleConfig.hpp"
#include "core/BarManager.hpp"
#include "render/Markup.hpp"
#include "render/TextCache.hpp"
#include "services/DBus.hpp"
#include "services/MainThread.hpp"
#include "services/Signals.hpp"
#include "util/Format.hpp"

#include <algorithm>
#include <string>

static SP<SHyprCtlCommand> g_ctlCommand;

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

using namespace Config::Values;

static void registerConfig() {
    auto& c = g_cfg;

    c.enabled  = makeShared<CBoolValue>("plugin:hyprstatus:enabled", "enable the bar", true);
    c.position = makeShared<CStringValue>("plugin:hyprstatus:position", "bar edge", "top",
                                          SStringValueOptions{.validator = strChoice({"top", "bottom"})});
    c.height     = makeShared<CIntValue>("plugin:hyprstatus:height", "bar height (logical px)", 30, SIntValueOptions{.min = 8, .max = 512});
    c.margin     = makeShared<CIntValue>("plugin:hyprstatus:margin", "outer gap around the bar", 0, SIntValueOptions{.min = 0, .max = 256});
    c.spacing    = makeShared<CIntValue>("plugin:hyprstatus:spacing", "gap between modules", 4, SIntValueOptions{.min = 0, .max = 128});
    c.padding    = makeShared<CIntValue>("plugin:hyprstatus:padding", "default module horizontal padding", 6, SIntValueOptions{.min = 0, .max = 128});
    c.rounding   = makeShared<CIntValue>("plugin:hyprstatus:rounding", "bar corner rounding", 0, SIntValueOptions{.min = 0, .max = 64});
    c.borderSize = makeShared<CIntValue>("plugin:hyprstatus:border_size", "bar border width", 0, SIntValueOptions{.min = 0, .max = 16});
    c.blur       = makeShared<CBoolValue>("plugin:hyprstatus:blur", "blur behind the bar", false);
    c.hideOnFullscreen = makeShared<CBoolValue>("plugin:hyprstatus:hide_on_fullscreen", "hide the bar when a window is fullscreen", true);
    // registered on every version so configs stay portable; only has an effect
    // on Hyprland >= 0.56 (custom plugin events), see BarManager overview interop
    c.hideOnOverview   = makeShared<CBoolValue>("plugin:hyprstatus:hide_on_overview", "hide the bar while a gloview overview is open on the monitor", true);
    c.tooltips   = makeShared<CBoolValue>("plugin:hyprstatus:tooltips", "enable tooltips", true);
    c.tooltipDelayMs = makeShared<CIntValue>("plugin:hyprstatus:tooltip_delay", "tooltip hover delay (ms)", 500, SIntValueOptions{.min = 0, .max = 10000});
    c.fontFamily = makeShared<CStringValue>("plugin:hyprstatus:font_family", "bar font", "Sans");
    c.fontSize   = makeShared<CIntValue>("plugin:hyprstatus:font_size", "font size (logical px)", 12, SIntValueOptions{.min = 6, .max = 64});

    c.modulesLeft   = makeShared<CStringValue>("plugin:hyprstatus:modules_left", "left modules", "workspaces window");
    c.modulesCenter = makeShared<CStringValue>("plugin:hyprstatus:modules_center", "center modules", "clock");
    c.modulesRight  = makeShared<CStringValue>("plugin:hyprstatus:modules_right", "right modules", "network pulseaudio battery");
    c.monitors      = makeShared<CStringValue>("plugin:hyprstatus:monitors", "monitors to show the bar on (empty = all)", "");
    c.iconTheme     = makeShared<CStringValue>("plugin:hyprstatus:icon_theme", "tray icon theme", "");
    c.trayIconSize  = makeShared<CIntValue>("plugin:hyprstatus:tray_icon_size", "tray icon size (logical px)", 20, SIntValueOptions{.min = 8, .max = 128});

    c.colBackground       = makeShared<CColorValue>("plugin:hyprstatus:col.background", "bar background", (Config::INTEGER)0xdd11111bLL);
    c.colForeground       = makeShared<CColorValue>("plugin:hyprstatus:col.foreground", "default text color", (Config::INTEGER)0xffc8c8d8LL);
    c.colForegroundBright = makeShared<CColorValue>("plugin:hyprstatus:col.foreground_bright", "hover/emphasis text", (Config::INTEGER)0xffffffffLL);
    c.colBorder           = makeShared<CColorValue>("plugin:hyprstatus:col.border", "bar border color", (Config::INTEGER)0xff3a3a4aLL);
    c.colAccent           = makeShared<CColorValue>("plugin:hyprstatus:col.accent", "accent color", (Config::INTEGER)0xff7aa2f7LL);
    c.colAccentDim        = makeShared<CColorValue>("plugin:hyprstatus:col.accent_dim", "dim accent", (Config::INTEGER)0xff4a6296LL);
    c.colOk               = makeShared<CColorValue>("plugin:hyprstatus:col.ok", "good state", (Config::INTEGER)0xff9ece6aLL);
    c.colWarn             = makeShared<CColorValue>("plugin:hyprstatus:col.warn", "warning state", (Config::INTEGER)0xffe0af68LL);
    c.colErr              = makeShared<CColorValue>("plugin:hyprstatus:col.err", "error/critical state", (Config::INTEGER)0xfff7768eLL);
    c.colTooltipBg        = makeShared<CColorValue>("plugin:hyprstatus:col.tooltip_bg", "tooltip background", (Config::INTEGER)0xf01e1e28LL);
    c.colTooltipFg        = makeShared<CColorValue>("plugin:hyprstatus:col.tooltip_fg", "tooltip text", (Config::INTEGER)0xffc8c8d8LL);

    const auto REG = [](SP<Config::Values::IValue> v) { HyprlandAPI::addConfigValueV2(PHANDLE, v); };
    for (auto& v : std::initializer_list<SP<Config::Values::IValue>>{
             c.enabled, c.position, c.height, c.margin, c.spacing, c.padding, c.rounding, c.borderSize, c.blur,
             c.hideOnFullscreen, c.hideOnOverview, c.tooltips, c.tooltipDelayMs, c.fontFamily, c.fontSize, c.modulesLeft, c.modulesCenter, c.modulesRight,
             c.monitors, c.iconTheme, c.trayIconSize, c.colBackground, c.colForeground, c.colForegroundBright,
             c.colBorder, c.colAccent, c.colAccentDim, c.colOk, c.colWarn, c.colErr, c.colTooltipBg, c.colTooltipFg})
        REG(v);
}

// Name the component behind an ABI-hash mismatch. The plugin hash is a COMPOSITE
// (see __hyprland_api_get_client_hash in PluginAPI.hpp):
//   <hyprland commit>_aq_<aquamarine>_hu_<hyprutils>_hg_<hyprgraphics>_hc_<hyprcursor>_hlg_<hyprlang>
// with each dependency stripped to major.minor. So a distro that ships a dependency
// minor bump BEFORE rebuilding Hyprland trips this even though the compositor commit
// matches, and the old "run hyprpm update" advice is then actively wrong: rebuilding
// only makes the plugin newer still. Report which token differs so the direction is
// obvious (plugin older -> rebuild the plugin; plugin newer -> Hyprland must be
// rebuilt against the new dependency, or the dependency downgraded).
static std::string abiMismatchDetail(const std::string& server, const std::string& client) {
    const auto S = Fmt::split(server, '_');
    const auto C = Fmt::split(client, '_');
    if (S.empty() || C.empty())
        return "";

    const auto SHORT = [](const std::string& h) { return h.size() > 10 ? h.substr(0, 10) : h; };
    if (S[0] != C[0])
        return "hyprland commit " + SHORT(C[0]) + " (plugin) != " + SHORT(S[0]) + " (running)";

    // remaining tokens are key/value pairs: aq 0.15 hu 0.14 ...
    std::string out;
    for (size_t i = 1; i + 1 < std::min(S.size(), C.size()); i += 2) {
        if (S[i] != C[i] || S[i + 1] != C[i + 1]) {
            if (!out.empty())
                out += ", ";
            out += C[i] + " " + C[i + 1] + " (plugin) != " + S[i + 1] + " (running)";
        }
    }
    return out;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH   = __hyprland_api_get_hash();
    const std::string CLIENT = __hyprland_api_get_client_hash();
    if (HASH != CLIENT) {
        const auto DETAIL = abiMismatchDetail(HASH, CLIENT);
        const auto WHAT   = DETAIL.empty() ? CLIENT + " (plugin) != " + HASH + " (running)" : DETAIL;
        HyprlandAPI::addNotification(PHANDLE, "[hyprstatus] ABI mismatch: " + WHAT + ". If the plugin is the older side run `hyprpm update`; if it is newer, Hyprland itself needs rebuilding against that dependency.",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 12000);
        throw std::runtime_error("[hyprstatus] version mismatch: " + WHAT);
    }

    MainThread::init();
    RtSignals::init();
    DBus::init(); // re-arm after a failed-dlclose re-init (see DBus.hpp)
    registerConfig();
    ModuleConfigStore::registerKeywords();

    g_barManager = makeUnique<CBarManager>();
    g_barManager->init();

    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprstatus:toggle", [](std::string) -> SDispatchResult {
        if (g_barManager)
            g_barManager->toggleVisible();
        return {};
    });
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprstatus:refresh", [](std::string arg) -> SDispatchResult {
        if (g_barManager)
            g_barManager->refreshModule(arg);
        return {};
    });

    g_ctlCommand = HyprlandAPI::registerHyprCtlCommand(
        PHANDLE, SHyprCtlCommand{.name = "hyprstatus", .exact = true, .fn = [](eHyprCtlOutputFormat fmt, std::string) -> std::string {
                                     return g_barManager ? g_barManager->statusJson() : "{}";
                                 }});

    HyprlandAPI::reloadConfig();

    return {"hyprstatus", "Compositor-rendered status bar (Waybar replacement)", "Tecknich", "1.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // order matters: modules (join threads, remove fd sources/timers) ->
    // reservations + pass elements -> shared services -> caches
    if (g_barManager) {
        g_barManager->shutdown();
        g_barManager.reset();
    }
    RtSignals::shutdown();
    DBus::shutdown();
    MainThread::shutdown();
    TextCache::clear();
    MarkupText::clear();
    g_ctlCommand.reset();
}
