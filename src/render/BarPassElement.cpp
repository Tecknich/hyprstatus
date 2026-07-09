#define WLR_USE_UNSTABLE
#include "BarPassElement.hpp"

#include <hyprland/src/helpers/Monitor.hpp>

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

    return BAR->barBoxLocal(); // monitor-local LOGICAL; simplify() scales it
}

bool CBarPassElement::needsLiveBlur() {
    // RASSERT: live blur with a null bounding box crashes the compositor
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
