#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>
#define WLR_USE_UNSTABLE
#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/event/EventBus.hpp>

#include "../modules/Module.hpp"
#include "../util/HyprCompat.hpp" // HS_HYPRLAND_056 gate
#include "Bar.hpp"

// Owns module instances, per-monitor bars, event listeners and the reserved
// areas. Everything here is main-thread only.
class CBarManager {
  public:
    void init();     // called from PLUGIN_INIT: registers listeners only.
                     // Bars/modules are built on the first config.reloaded.
    void shutdown(); // PLUGIN_EXIT: modules -> reservations -> pass elements -> damage

    // teardown + rebuild modules and bars from current config (config.reloaded)
    void rebuild();

    // reserved area management (RESERVED_DYNAMIC_TYPE_ERROR_BAR slot)
    void applyReservedAll();
    void clearReservedAll();

    // render hook (Event::bus render.stage)
    void onRenderStage(eRenderStage stage);

    // input hooks
    void onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info);
    void onMouseAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info);
    void onMouseMove(const Vector2D& pos);

    IModule* moduleByName(const std::string& name);
    void     refreshModule(const std::string& name); // "" = all
    // relayout + damage all bars (modules call IModule::requestRedraw -> here)
    void        requestRedraw();
    std::string statusJson() const; // hyprctl hyprstatus

    bool visible() const; // config enabled && runtime toggle
    void toggleVisible();

    bool wantsBarOnMonitor(const PHLMONITOR& mon) const;
    CBar* barForMonitor(const PHLMONITOR& mon);

    // true while a gloview overview is open on this monitor and
    // hide_on_overview is set — the bar is not drawn there, exactly like the
    // hide_on_fullscreen gate. Driven by the "gloview:overview" custom bus
    // event (Hyprland >= 0.56); always false on 0.55.
    bool overviewHidden(const PHLMONITOR& mon) const;

    struct SLayout {
        std::vector<IModule*> left, center, right;
    } m_layout;

    std::vector<UP<IModule>>     m_modules;
    std::map<uint64_t, UP<CBar>> m_bars; // key: monitor id

  private:
    void buildBars();      // create CBar for each wanted monitor
    void applyReserved(const PHLMONITOR& mon);

    // first module (if any) with an open native popup menu
    IModule* moduleWithPopup();

    // Pointer-cursor feedback over clickable bar segments. POINTER/DEFAULT are
    // only ever set while the cursor is over a bar (or an open popup); NONE
    // means "off all bars" and never touches the cursor — the compositor owns
    // it there. Transitions call setCursorFromName only on a state change.
    enum eCursorState { CURSOR_NONE, CURSOR_POINTER, CURSOR_DEFAULT };
    void         applyCursor(eCursorState want);
    eCursorState m_cursorState = CURSOR_NONE;

    bool m_runtimeVisible = true;
    bool m_built          = false;

    // pointer buttons whose PRESS the bar swallowed; a RELEASE is only consumed
    // when its press is in here, so releases for presses that went to a window
    // (e.g. a mod+drag ending over the bar) are never stolen.
    std::unordered_set<uint32_t> m_consumedButtons;

    // listeners (dropping = unsubscribing)
    CHyprSignalListener m_lRenderStage, m_lMonAdded, m_lMonRemoved, m_lMonLayout,
        m_lCfgPreReload, m_lCfgReloaded, m_lMouseButton, m_lMouseAxis, m_lMouseMove, m_lFullscreen;

#ifdef HS_HYPRLAND_056
    // ---- gloview overview interop (custom plugin bus events) ----
    // LIFETIME RULE: hold ONLY CHyprSignalListeners here, never the
    // SP<CCustomEvent> — co-owning gloview's event object would run its
    // deleter (code inside gloview's .so) after gloview is dlclosed and crash
    // the compositor. The SP is used transiently in subscribeOverview only.
    void subscribeOverview(const SP<Event::CEventBus::CCustomEvent>& ev);
    void onOverviewEvent(const std::vector<Event::CEventBus::CCustomEvent::ValidVariant>& args);
    void clearOverviewState(); // forget all open overviews + damage (unhide bars)

    std::unordered_set<uint64_t> m_overviewOpen;     // monitor ids with an open overview
    CHyprSignalListener          m_lOverviewEvent;   // "gloview:overview" payload
    CHyprSignalListener          m_lPluginEvAdded;   // gloview loads after us / reloads
    CHyprSignalListener          m_lPluginEvRemoved; // gloview unloads
#endif
};
