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

    struct SLayout {
        std::vector<IModule*> left, center, right;
    } m_layout;

    std::vector<UP<IModule>>     m_modules;
    std::map<uint64_t, UP<CBar>> m_bars; // key: monitor id

  private:
    void buildBars();      // create CBar for each wanted monitor
    void applyReserved(const PHLMONITOR& mon);

    bool m_runtimeVisible = true;
    bool m_built          = false;

    // pointer buttons whose PRESS the bar swallowed; a RELEASE is only consumed
    // when its press is in here, so releases for presses that went to a window
    // (e.g. a mod+drag ending over the bar) are never stolen.
    std::unordered_set<uint32_t> m_consumedButtons;

    // listeners (dropping = unsubscribing)
    CHyprSignalListener m_lRenderStage, m_lMonAdded, m_lMonRemoved, m_lMonLayout,
        m_lCfgPreReload, m_lCfgReloaded, m_lMouseButton, m_lMouseAxis, m_lMouseMove, m_lFullscreen;
};
