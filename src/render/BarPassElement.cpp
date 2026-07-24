#define WLR_USE_UNSTABLE
#include "BarPassElement.hpp"

// 0.56 moved CMonitor: helpers/Monitor.hpp -> output/Monitor.hpp
#if __has_include(<hyprland/src/output/Monitor.hpp>)
#include <hyprland/src/output/Monitor.hpp>
#else
#include <hyprland/src/helpers/Monitor.hpp>
#endif

#include "../core/Bar.hpp"
#include "../core/BarManager.hpp"
#include "../globals.hpp"

std::vector<UP<IPassElement>> CBarPassElement::draw() {
    const auto MON = m_monitor.lock();
    if (!MON || !g_barManager)
        return {};

    if (const auto BAR = g_barManager->barForMonitor(MON))
        BAR->draw();

    return {};
}

std::optional<CBox> CBarPassElement::boundingBox() {
    const auto MON = m_monitor.lock();
    if (!MON || !g_barManager)
        return std::nullopt;

    const auto BAR = g_barManager->barForMonitor(MON);
    if (!BAR)
        return std::nullopt;

    const auto BOX = BAR->barBoxLocal(); // monitor-local LOGICAL; simplify() scales it
    // A degenerate (zero/negative width or height) box must never feed the live
    // blur path: negative geometry into blur is UB and Hyprland RASSERTs on it.
    // Reporting nullopt here makes needsLiveBlur() (which is gated on
    // boundingBox().has_value()) return false, so the pair can't hit the
    // "needsLiveBlur() && !boundingBox()" RASSERT either.
    if (BOX.w <= 0 || BOX.h <= 0)
        return std::nullopt;
    return BOX;
}

bool CBarPassElement::needsLiveBlur() {
    // Gate on a valid, non-degenerate box: boundingBox() returns nullopt for a
    // degenerate bar, and live blur with a null/negative bounding box RASSERTs
    // (crashes the compositor).
    return g_cfg.blur && g_cfg.blur->value() && boundingBox().has_value();
}

std::vector<UP<IPassElement>> CTooltipPassElement::draw() {
    const auto MON = m_monitor.lock();
    if (!MON || !g_barManager)
        return {};

    if (const auto BAR = g_barManager->barForMonitor(MON))
        BAR->drawTooltip();

    return {};
}

std::vector<UP<IPassElement>> CPopupPassElement::draw() {
    const auto MON = m_monitor.lock();
    if (!MON || !g_barManager)
        return {};

    // draw the popup owned by whichever module has one open on this monitor
    for (auto& mod : g_barManager->m_modules) {
        if (mod && mod->popupOpen() && mod->popupMonitor().lock() == MON) {
            mod->drawPopup(MON);
            break;
        }
    }

    return {};
}
