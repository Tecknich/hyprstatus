#pragma once
#include <vector>
#define WLR_USE_UNSTABLE
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/math/Math.hpp>

#include "../modules/Module.hpp"

// Region of the bar owned by one module segment; coordinates are
// monitor-local LOGICAL pixels (pre-scale).
struct SHitRegion {
    CBox     box;
    IModule* module = nullptr;
    SSegment segment; // copy taken at layout time, used for click routing
};

// One bar per monitor. Layout is computed in monitor-local logical pixels;
// scaling by monitor->m_scale happens only inside draw().
class CBar {
  public:
    explicit CBar(PHLMONITOR mon);

    PHLMONITORREF m_monitor;

    // geometry (logical)
    CBox   barBoxLocal() const;  // monitor-local, margins applied
    CBox   barBoxGlobal() const; // global logical (hit tests, damage)
    double reservedLogical() const; // height + margins to reserve

    // Full render for this monitor. ONLY call from inside a pass element's
    // draw() (GL context current). Rebuilds m_hitRegions as a side effect.
    void draw();

    // Render the tooltip for the currently hovered region, if due.
    // Called from the tooltip pass element (RENDER_LAST_MOMENT).
    void drawTooltip();

    // input helpers (global logical coords)
    bool hitTest(const Vector2D& global, int* regionIdx) const;

    void damage() const; // damageBox(global bar box) — schedules a frame

    // hover state (managed by CBarManager::onMouseMove)
    int      m_hoveredIdx   = -1; // index into m_hitRegions, -1 = none
    uint64_t m_hoverStartMs = 0;  // steady ms when hover began (tooltip delay)

    std::vector<SHitRegion> m_hitRegions; // rebuilt by draw()
};
