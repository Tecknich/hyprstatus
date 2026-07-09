#define WLR_USE_UNSTABLE
#include "globals.hpp"

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/event/EventBus.hpp>

#include "config/ModuleConfig.hpp"
#include "core/BarManager.hpp"
#include "render/TextCache.hpp"
#include "services/DBus.hpp"
#include "services/MainThread.hpp"
#include "services/Signals.hpp"

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

    for (auto& v : {(SP<Config::Values::IValue>)c.enabled, c.position, c.height, c.margin, c.spacing, c.padding,
                    c.rounding, c.borderSize, c.blur, c.tooltips, c.tooltipDelayMs, c.fontFamily, c.fontSize,
                    c.modulesLeft, c.modulesCenter, c.modulesRight, c.monitors, c.iconTheme, c.trayIconSize,
                    c.colBackground, c.colForeground, c.colForegroundBright, c.colBorder, c.colAccent,
                    c.colAccentDim, c.colOk, c.colWarn, c.colErr, c.colTooltipBg, c.colTooltipFg})
        HyprlandAPI::addConfigValueV2(PHANDLE, v);
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH = __hyprland_api_get_hash();
    if (HASH != __hyprland_api_get_client_hash()) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprstatus] Version mismatch (headers != running Hyprland). Run `hyprpm update`.",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
        throw std::runtime_error("[hyprstatus] version mismatch");
    }

    MainThread::init();
    RtSignals::init();
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

    return {"hyprstatus", "Compositor-rendered status bar (Waybar replacement)", "Tecknich", "0.1.0"};
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
    g_ctlCommand.reset();
}
