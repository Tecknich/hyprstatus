#define WLR_USE_UNSTABLE
#include "BarManager.hpp"

#include <chrono>
#include <sstream>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/reserved/ReservedArea.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include "../globals.hpp"
#include "../config/ModuleConfig.hpp"
#include "../modules/Factories.hpp"
#include "../render/BarPassElement.hpp"
#include "../render/TextCache.hpp"
#include "../services/Signals.hpp"
#include "../util/Json.hpp"

// Only one CBarManager ever exists (g_barManager); the header exposes no slots
// for these, so they live file-local. Torn down in shutdown().
static SP<CEventLoopTimer>       g_tooltipTimer;   // disarmed unless a region is hovered
static UP<SEventLoopDoLaterLock> g_reservedLater;  // coalesces layoutChanged bursts; RAII-cancelled on unload
static UP<SEventLoopDoLaterLock> g_rebuildLater;

static std::vector<std::string> splitWs(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream       ss(s);
    std::string              tok;
    while (ss >> tok)
        out.push_back(tok);
    return out;
}

// The tooltip is drawn just beyond the bar (a small gap past it) and clamped
// on-monitor, so it lives OUTSIDE barBoxGlobal(): bar->damage() alone never
// repaints it, and pass-element GL is clipped to the frame damage, so an
// undamaged tooltip box is never painted (and, once painted, never erased).
// Damage the whole strip from the bar to the monitor edge on the tooltip side
// (bar box included) whenever a tooltip appears or is dismissed.
static void damageTooltipBand(const CBar* bar) {
    if (!bar)
        return;

    const auto MON = bar->m_monitor.lock();
    const CBox BG  = bar->barBoxGlobal();
    if (!MON) {
        g_pHyprRenderer->damageBox(BG);
        return;
    }

    const double MONX = MON->m_position.x;
    const double MONY = MON->m_position.y;
    const double MONW = MON->m_size.x;
    const double MONH = MON->m_size.y;

    CBox band;
    if (g_cfg.position->value() == "bottom") // tooltip is above the bar
        band = CBox{MONX, MONY, MONW, (BG.y + BG.h) - MONY};
    else                                     // tooltip is below the bar
        band = CBox{MONX, BG.y, MONW, (MONY + MONH) - BG.y};

    g_pHyprRenderer->damageBox(band);
}

void CBarManager::init() {
    auto& events = Event::bus()->m_events;

    m_lRenderStage = events.render.stage.listen([this](eRenderStage stage) { onRenderStage(stage); });

    m_lMonAdded = events.monitor.added.listen([this](PHLMONITOR mon) {
        if (!m_built || !mon)
            return;
        if (wantsBarOnMonitor(mon) && !mon->isMirror())
            m_bars[mon->m_id] = makeUnique<CBar>(mon);
        applyReserved(mon);
        if (auto* const BAR = barForMonitor(mon))
            BAR->damage();
    });

    m_lMonRemoved = events.monitor.removed.listen([this](PHLMONITOR mon) {
        if (!mon)
            return;
        m_bars.erase(mon->m_id);
    });

    // applyMonitorRule wipes the dynamic reserved slots and emits this event;
    // re-applying synchronously would re-enter arrangeLayersForMonitor, so defer.
    m_lMonLayout = events.monitor.layoutChanged.listen([this] {
        g_reservedLater = g_pEventLoopManager->doLaterLock([this] { applyReservedAll(); });
    });

    m_lCfgPreReload = events.config.preReload.listen([] { ModuleConfigStore::clear(); });

    m_lCfgReloaded = events.config.reloaded.listen([this] {
        g_rebuildLater = g_pEventLoopManager->doLaterLock([this] { rebuild(); });
    });

    m_lMouseButton = events.input.mouse.button.listen(
        [this](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onMouseButton(e, info); });

    m_lMouseAxis = events.input.mouse.axis.listen(
        [this](IPointer::SAxisEvent e, Event::SCallbackInfo& info) { onMouseAxis(e, info); });

    // observe only: cancelling move events would break focus-follows-mouse
    m_lMouseMove = events.input.mouse.move.listen(
        [this](Vector2D pos, Event::SCallbackInfo&) { onMouseMove(pos); });

    // one-shot, armed on hover start; fires once the tooltip delay elapses and
    // damages the hovered bar so a frame renders with the tooltip element.
    g_tooltipTimer = makeShared<CEventLoopTimer>(
        std::nullopt,
        [](SP<CEventLoopTimer>, void*) {
            if (!g_barManager)
                return;
            // never surface a tooltip over the session lockscreen
            if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked())
                return;
            for (auto& [id, bar] : g_barManager->m_bars) {
                if (bar->m_hoveredIdx != -1)
                    damageTooltipBand(bar.get());
            }
        },
        nullptr);
    g_pEventLoopManager->addTimer(g_tooltipTimer);
}

void CBarManager::shutdown() {
    m_layout = {};
    m_modules.clear(); // FIRST: module dtors join threads / remove timers and fd sources
    m_bars.clear();
    clearReservedAll();

    g_pHyprRenderer->m_renderPass.removeAllOfType("CHyprstatusBarPassElement");
    g_pHyprRenderer->m_renderPass.removeAllOfType("CHyprstatusTooltipPassElement");

    if (g_tooltipTimer) {
        g_pEventLoopManager->removeTimer(g_tooltipTimer);
        g_tooltipTimer.reset();
    }
    g_reservedLater.reset();
    g_rebuildLater.reset();

    m_lRenderStage.reset();
    m_lMonAdded.reset();
    m_lMonRemoved.reset();
    m_lMonLayout.reset();
    m_lCfgPreReload.reset();
    m_lCfgReloaded.reset();
    m_lMouseButton.reset();
    m_lMouseAxis.reset();
    m_lMouseMove.reset();

    m_built = false;

    for (auto& mon : g_pCompositor->m_monitors)
        g_pHyprRenderer->damageMonitor(mon);
}

void CBarManager::rebuild() {
    m_bars.clear();
    m_layout = {};
    m_modules.clear();
    RtSignals::unsubscribeAll();
    TextCache::clear();

    // same name in two groups reuses the one instance; nullptr = unknown
    // (notified once), skipped everywhere it appears.
    std::map<std::string, IModule*> byName;

    const auto INSTANTIATE = [&](const std::string& name) -> IModule* {
        if (const auto IT = byName.find(name); IT != byName.end())
            return IT->second;

        SModuleConfig cfg = ModuleConfigStore::get(name);
        cfg.name          = name;

        auto mod = createModuleByName(cfg);
        if (!mod) {
            HyprlandAPI::addNotification(PHANDLE, "[hyprstatus] unknown module: " + name, CHyprColor{1.0, 0.6, 0.2, 1.0}, 5000);
            byName[name] = nullptr;
            return nullptr;
        }

        auto* const RAW = mod.get();
        m_modules.emplace_back(std::move(mod));
        byName[name] = RAW;
        return RAW;
    };

    for (const auto& name : splitWs(g_cfg.modulesLeft->value()))
        if (auto* const MOD = INSTANTIATE(name))
            m_layout.left.push_back(MOD);
    for (const auto& name : splitWs(g_cfg.modulesCenter->value()))
        if (auto* const MOD = INSTANTIATE(name))
            m_layout.center.push_back(MOD);
    for (const auto& name : splitWs(g_cfg.modulesRight->value()))
        if (auto* const MOD = INSTANTIATE(name))
            m_layout.right.push_back(MOD);

    // init only after ALL modules are constructed: init() may emit updates that
    // relayout bars and touch sibling modules through the layout.
    for (auto& mod : m_modules)
        mod->init();

    buildBars();
    applyReservedAll();
    requestRedraw();

    m_built = true;
}

void CBarManager::buildBars() {
    m_bars.clear();
    for (auto& mon : g_pCompositor->m_monitors) {
        if (!mon || mon->isMirror())
            continue;
        if (wantsBarOnMonitor(mon))
            m_bars[mon->m_id] = makeUnique<CBar>(mon);
    }
}

bool CBarManager::wantsBarOnMonitor(const PHLMONITOR& mon) const {
    if (!mon)
        return false;
    const auto LIST = g_cfg.monitors->value();
    if (LIST.empty())
        return true;
    for (const auto& name : splitWs(LIST))
        if (name == mon->m_name)
            return true;
    return false;
}

void CBarManager::applyReserved(const PHLMONITOR& mon) {
    if (!mon)
        return;

    auto& RA = mon->m_reservedArea;
    RA.resetType(Desktop::RESERVED_DYNAMIC_TYPE_ERROR_BAR); // addType accumulates; reset keeps this idempotent

    const auto* const BAR = barForMonitor(mon);
    if (visible() && BAR) {
        const double RESERVED = BAR->reservedLogical(); // logical px, config values are logical already
        if (g_cfg.position->value() == "bottom")
            RA.addType(Desktop::RESERVED_DYNAMIC_TYPE_ERROR_BAR, Vector2D{0.0, 0.0}, Vector2D{0.0, RESERVED});
        else
            RA.addType(Desktop::RESERVED_DYNAMIC_TYPE_ERROR_BAR, Vector2D{0.0, RESERVED}, Vector2D{0.0, 0.0});
    }

    g_pHyprRenderer->arrangeLayersForMonitor(mon->m_id); // retiles + damages
}

void CBarManager::applyReservedAll() {
    for (auto& mon : g_pCompositor->m_monitors)
        applyReserved(mon);
}

void CBarManager::clearReservedAll() {
    for (auto& mon : g_pCompositor->m_monitors) {
        if (!mon)
            continue;
        mon->m_reservedArea.resetType(Desktop::RESERVED_DYNAMIC_TYPE_ERROR_BAR);
        g_pHyprRenderer->arrangeLayersForMonitor(mon->m_id);
    }
}

void CBarManager::onRenderStage(eRenderStage stage) {
    if (stage == RENDER_POST_WINDOWS) {
        if (!visible())
            return;

        const auto PMONITOR = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!PMONITOR || !barForMonitor(PMONITOR))
            return;

        g_pHyprRenderer->m_renderPass.add(makeUnique<CBarPassElement>(PMONITOR));
        return;
    }

    if (stage != RENDER_LAST_MOMENT)
        return;

    if (!visible() || !g_cfg.tooltips->value())
        return;

    // tooltips render at RENDER_LAST_MOMENT, above the lockscreen surface — do
    // not leak bar tooltip content over a locked session.
    if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked())
        return;

    const auto PMONITOR = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!PMONITOR)
        return;

    auto* const BAR = barForMonitor(PMONITOR);
    if (!BAR || BAR->m_hoveredIdx < 0 || (size_t)BAR->m_hoveredIdx >= BAR->m_hitRegions.size())
        return;

    auto& region = BAR->m_hitRegions[BAR->m_hoveredIdx];
    if (!region.module || region.module->tooltip(region.segment).empty())
        return;

    const uint64_t NOW = Time::millis(Time::steadyNow());
    if (NOW - BAR->m_hoverStartMs < (uint64_t)g_cfg.tooltipDelayMs->value())
        return;

    g_pHyprRenderer->m_renderPass.add(makeUnique<CTooltipPassElement>(PMONITOR));
}

// bar under the cursor, or nullptr. Coordinates are global logical.
static CBar* barAt(std::map<uint64_t, UP<CBar>>& bars, const Vector2D& pos) {
    for (auto& [id, bar] : bars) {
        if (bar->barBoxGlobal().containsPoint(pos))
            return bar.get();
    }
    return nullptr;
}

// true when a seat grab or a top/overlay layer-surface owns this point — the
// bar renders below those, so it must not steal their input.
static bool pointClaimedAbove(const Vector2D& pos, const PHLMONITOR& mon) {
    if (g_pSeatManager->m_seatGrab)
        return true;

    PHLLS    ls;
    Vector2D surfaceCoords;
    if (g_pCompositor->vectorToLayerSurface(pos, &mon->m_layerSurfaceLayers[3 /* overlay */], &surfaceCoords, &ls))
        return true;
    if (g_pCompositor->vectorToLayerSurface(pos, &mon->m_layerSurfaceLayers[2 /* top */], &surfaceCoords, &ls))
        return true;

    return false;
}

void CBarManager::onMouseButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
    // While the session is locked, InputManager emits this before any lock
    // routing: consuming it would eat clicks meant for the lock surface, and
    // routing it would fire module onClick handlers (workspace switch, spawn())
    // on a locked machine. Never touch the event.
    if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked()) {
        m_consumedButtons.clear();
        return;
    }

    const bool PRESS = e.state == WL_POINTER_BUTTON_STATE_PRESSED;

    // A RELEASE is only ours if we swallowed its PRESS. Decide this independent
    // of cursor position so the press/release stay balanced even when the button
    // is released off the bar, and a release whose press went to a window (e.g. a
    // mod+drag ending over the bar) is never stolen — stealing it wedges the drag.
    if (!PRESS) {
        if (m_consumedButtons.erase(e.button))
            info.cancelled = true;
        return;
    }

    if (!visible())
        return;

    const auto  MOUSE = g_pInputManager->getMouseCoordsInternal();
    auto* const BAR   = barAt(m_bars, MOUSE);
    if (!BAR)
        return;

    const auto PMONITOR = BAR->m_monitor.lock();
    if (!PMONITOR)
        return;

    // Bar auto-hides over a solitary fullscreen window (RENDER_POST_WINDOWS is
    // skipped that frame): it is not drawn, so it must not consume or route the
    // click — that would eat in-app clicks and fire stale hit-region actions.
    if (!PMONITOR->m_solitaryClient.expired())
        return;

    if (pointClaimedAbove(MOUSE, PMONITOR))
        return; // do not consume

    info.cancelled = true; // swallow the press before keybinds / window delivery
    m_consumedButtons.insert(e.button);

    int regionIdx = -1;
    if (!BAR->hitTest(MOUSE, &regionIdx))
        return;

    auto& region = BAR->m_hitRegions[regionIdx];
    if (region.module)
        region.module->onClick(e.button, region.segment, PMONITOR);
}

void CBarManager::onMouseAxis(const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) {
    // Never consume or route scroll on a locked session (see onMouseButton).
    if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked())
        return;

    if (!visible())
        return;

    const auto  MOUSE = g_pInputManager->getMouseCoordsInternal();
    auto* const BAR   = barAt(m_bars, MOUSE);
    if (!BAR)
        return;

    const auto PMONITOR = BAR->m_monitor.lock();
    if (!PMONITOR)
        return;

    // Not drawn over a solitary fullscreen window: do not swallow the scroll
    // (else a scroll on a fullscreen app silently dispatches workspace +1/-1).
    if (!PMONITOR->m_solitaryClient.expired())
        return;

    if (pointClaimedAbove(MOUSE, PMONITOR))
        return;

    info.cancelled = true; // only when inside the bar box

    int regionIdx = -1;
    if (!BAR->hitTest(MOUSE, &regionIdx))
        return;

    auto& region = BAR->m_hitRegions[regionIdx];
    if (region.module)
        region.module->onScroll(e.delta, region.segment, PMONITOR);
}

void CBarManager::onMouseMove(const Vector2D& pos) {
    CBar* newBar = nullptr;
    int   newIdx = -1;

    if (visible()) {
        if ((newBar = barAt(m_bars, pos)))
            newBar->hitTest(pos, &newIdx);
    }
    if (newIdx == -1)
        newBar = nullptr; // over bar background but no module segment = no hover

    CBar* oldBar = nullptr;
    for (auto& [id, bar] : m_bars) {
        if (bar->m_hoveredIdx != -1) {
            oldBar = bar.get();
            break;
        }
    }

    if (oldBar == newBar && (!newBar || newBar->m_hoveredIdx == newIdx))
        return; // hover unchanged

    // If the old hover lasted long enough for its tooltip to have painted,
    // damage the tooltip band so it is erased — the tooltip lives outside the
    // bar box, which bar->damage() alone would leave as a ghost.
    if (oldBar && g_cfg.tooltips->value() &&
        Time::millis(Time::steadyNow()) - oldBar->m_hoverStartMs >= (uint64_t)g_cfg.tooltipDelayMs->value())
        damageTooltipBand(oldBar);

    if (oldBar && oldBar != newBar) {
        oldBar->m_hoveredIdx = -1;
        oldBar->damage();
    }

    if (newBar) {
        newBar->m_hoveredIdx   = newIdx;
        newBar->m_hoverStartMs = Time::millis(Time::steadyNow());
        newBar->damage();
        if (g_cfg.tooltips->value() && g_tooltipTimer)
            g_tooltipTimer->updateTimeout(std::chrono::milliseconds(g_cfg.tooltipDelayMs->value()));
    } else if (g_tooltipTimer)
        g_tooltipTimer->updateTimeout(std::nullopt); // left all bars: disarm
}

IModule* CBarManager::moduleByName(const std::string& name) {
    for (auto& mod : m_modules) {
        if (mod->name() == name)
            return mod.get();
    }
    return nullptr;
}

void CBarManager::refreshModule(const std::string& name) {
    if (name.empty()) {
        for (auto& mod : m_modules)
            mod->update();
        return;
    }
    if (auto* const MOD = moduleByName(name))
        MOD->update();
}

void CBarManager::requestRedraw() {
    for (auto& [id, bar] : m_bars)
        bar->damage();
}

std::string CBarManager::statusJson() const {
    PHLMONITOR mon;
    if (const auto FOCUS = Desktop::focusState())
        mon = FOCUS->monitor();
    if (!mon) {
        for (const auto& [id, bar] : m_bars) {
            if ((mon = bar->m_monitor.lock()))
                break;
        }
    }

    std::string out = "{\"visible\":";
    out += visible() ? "true" : "false";
    out += ",\"modules\":[";

    bool first = true;
    for (const auto& mod : m_modules) {
        std::string text;
        if (mon) {
            for (const auto& seg : mod->segments(mon)) {
                if (!text.empty() && !seg.text.empty())
                    text += " ";
                text += seg.text;
            }
        }
        if (!first)
            out += ",";
        first = false;
        out += "{\"name\":\"" + MiniJson::escape(mod->name()) + "\",\"text\":\"" + MiniJson::escape(text) + "\"}";
    }

    out += "]}";
    return out;
}

bool CBarManager::visible() const {
    return g_cfg.enabled->value() && m_runtimeVisible;
}

void CBarManager::toggleVisible() {
    m_runtimeVisible = !m_runtimeVisible;
    applyReservedAll();
    for (auto& mon : g_pCompositor->m_monitors)
        g_pHyprRenderer->damageMonitor(mon);
}

CBar* CBarManager::barForMonitor(const PHLMONITOR& mon) {
    if (!mon)
        return nullptr;
    const auto IT = m_bars.find(mon->m_id);
    return IT == m_bars.end() ? nullptr : IT->second.get();
}
