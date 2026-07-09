#pragma once
#include <optional>
#include <string>
#include <vector>
#define WLR_USE_UNSTABLE
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/render/Texture.hpp>

#include "../config/ModuleConfig.hpp"

// One visual unit inside a module. Most modules render a single text segment;
// workspaces render one per workspace button, tray one icon per item.
struct SSegment {
    std::string               text;         // rendered via TextCache when icon is null
    SP<Render::ITexture>      icon;         // pre-rendered icon (tray); drawn square
    std::optional<CHyprColor> fg;           // hard color override
    std::string               cls;          // style class -> color resolution (active, warning, ...)
    std::string               tooltip;      // per-segment tooltip ("" -> module-level tooltip)
    size_t                    id    = 0;    // click identity (workspace id, tray index, ...)
    bool                      bold  = false;
    bool                      hoverable = false; // gets col.foreground_bright on hover (workspaces)
};

class IModule {
  public:
    explicit IModule(const SModuleConfig& cfg) : m_config(cfg) {}
    virtual ~IModule() = default;

    // start timers / listeners / services. Called once after construction on
    // the main thread. Config is fully parsed at this point.
    virtual void init() {}
    // forced data refresh (dispatcher `hyprstatus:refresh`, RT signal, poll tick)
    virtual void update() {}

    // Current render model for a given monitor (main thread, may be called
    // every frame — must be cheap: return cached state, do NOT do I/O here).
    virtual std::vector<SSegment> segments(PHLMONITOR mon) = 0;
    virtual bool                  hidden(PHLMONITOR mon) { return false; }

    // Tooltip text for a segment; "" = no tooltip. Default resolves the
    // segment tooltip, then the "tooltip-format"/"tooltip" options.
    virtual std::string tooltip(const SSegment& seg);

    // Input. Defaults spawn the on-click*/on-scroll-* option commands.
    virtual void onClick(uint32_t button, const SSegment& seg, PHLMONITOR mon);
    virtual void onScroll(double delta, const SSegment& seg, PHLMONITOR mon);

    // ---- native rendered popup menu (tray dbusmenu) ----
    // A module may own at most one popup. All of these run on the main thread.
    // Defaults are no-ops so non-popup modules are unaffected.
    virtual bool          popupOpen() const { return false; }
    virtual PHLMONITORREF popupMonitor() const { return {}; }
    // called inside a pass element at RENDER_LAST_MOMENT with GL current
    virtual void drawPopup(PHLMONITOR mon) {}
    // true = the click was inside the popup and was consumed
    virtual bool popupHandleButton(uint32_t button, const Vector2D& global) { return false; }
    virtual void popupHandleMotion(const Vector2D& global) {}
    virtual void closePopup() {}

    const std::string& name() const { return m_config.name; }

    // waybar-compatible option access
    std::string opt(const std::string& key, const std::string& fallback = "") const;
    int64_t     optInt(const std::string& key, int64_t fallback) const;
    double      optFloat(const std::string& key, double fallback) const;
    bool        optBool(const std::string& key, bool fallback) const;
    bool        hasOpt(const std::string& key) const;

  protected:
    // mark bars dirty: relayout + damage every bar (thread-safe? NO — main
    // thread only; workers go through MainThread::post first)
    void requestRedraw();
    // detached shell command via the compositor's executor
    void spawn(const std::string& cmd);

    SModuleConfig m_config;
};

// RAII periodic timer on the compositor event loop (self-rearming).
// Construct/destroy on the main thread only.
#include <chrono>
#include <functional>
class CModuleTimer {
  public:
    CModuleTimer(std::chrono::milliseconds interval, std::function<void()> onTick);
    ~CModuleTimer();
    void setInterval(std::chrono::milliseconds interval); // takes effect on next re-arm
    void fireNow();                                       // run onTick + re-arm

  private:
    struct SImpl;
    SP<SImpl> m_impl;
};
