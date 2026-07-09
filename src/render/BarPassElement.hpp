#pragma once
#define WLR_USE_UNSTABLE
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>

// Queued at RENDER_POST_WINDOWS for the monitor currently being rendered.
// draw() delegates to the monitor's CBar.
class CBarPassElement : public IPassElement {
  public:
    explicit CBarPassElement(PHLMONITORREF mon) : m_monitor(mon) {}
    virtual ~CBarPassElement() = default;

    virtual std::vector<UP<IPassElement>> draw() override;
    virtual bool                          needsLiveBlur() override;
    virtual bool                          needsPrecomputeBlur() override { return false; }
    virtual std::optional<CBox>           boundingBox() override; // monitor-local LOGICAL
    virtual CRegion                       opaqueRegion() override { return {}; }
    virtual const char*                   passName() override { return "CHyprstatusBarPassElement"; }
    virtual ePassElementType              type() override { return EK_CUSTOM; }

    PHLMONITORREF m_monitor;
};

// Queued at RENDER_LAST_MOMENT on the hovered monitor while a tooltip is due.
class CTooltipPassElement : public IPassElement {
  public:
    explicit CTooltipPassElement(PHLMONITORREF mon) : m_monitor(mon) {}
    virtual ~CTooltipPassElement() = default;

    virtual std::vector<UP<IPassElement>> draw() override;
    virtual bool                          needsLiveBlur() override { return false; }
    virtual bool                          needsPrecomputeBlur() override { return false; }
    virtual std::optional<CBox>           boundingBox() override { return std::nullopt; }
    virtual const char*                   passName() override { return "CHyprstatusTooltipPassElement"; }
    virtual ePassElementType              type() override { return EK_CUSTOM; }

    PHLMONITORREF m_monitor;
};

// Queued at RENDER_LAST_MOMENT while a module has an open popup on this monitor.
// draw() delegates to the popup-owning module's drawPopup().
class CPopupPassElement : public IPassElement {
  public:
    explicit CPopupPassElement(PHLMONITORREF mon) : m_monitor(mon) {}
    virtual ~CPopupPassElement() = default;

    virtual std::vector<UP<IPassElement>> draw() override;
    virtual bool                          needsLiveBlur() override { return false; }
    virtual bool                          needsPrecomputeBlur() override { return false; }
    virtual std::optional<CBox>           boundingBox() override { return std::nullopt; }
    virtual const char*                   passName() override { return "CHyprstatusPopupPassElement"; }
    virtual ePassElementType              type() override { return EK_CUSTOM; }

    PHLMONITORREF m_monitor;
};
