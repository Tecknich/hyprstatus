#pragma once
// Hyprland 0.55 -> 0.56 API compatibility.
//
// 0.56 moved compositor-owned state into dedicated trackers:
//   g_pCompositor->m_monitors            -> State::monitorState()->monitors()
//   g_pCompositor->getWorkspacesCopy()   -> State::workspaceState()->workspacesCopy()
//   g_pCompositor->getWorkspaceByID(id)  -> State::workspaceState()->query().id(id).run()
//   CMonitor::inFullscreenMode()         -> Fullscreen::controller()->hasFullscreen(mon)
//   g_pCompositor->vectorToLayerSurface  -> Desktop::viewState()->hitTest().layerSurfaceAt(...)
//
// Detected by header presence (not version macros) so ONE tree builds against
// both the 0.55.4 headers Arch currently ships and 0.56+; callers must only
// use the Compat:: wrappers below. Drop the 0.55 branches when the 0.55.4
// commit_pin makes them unreachable.
#include <hyprland/src/Compositor.hpp>

#if __has_include(<hyprland/src/state/MonitorState.hpp>)
#define HS_HYPRLAND_056 1
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#endif

namespace Compat {
    inline const std::vector<PHLMONITOR>& monitors() {
#ifdef HS_HYPRLAND_056
        return State::monitorState()->monitors();
#else
        return g_pCompositor->m_monitors;
#endif
    }

    inline std::vector<PHLWORKSPACE> workspacesCopy() {
#ifdef HS_HYPRLAND_056
        return State::workspaceState()->workspacesCopy();
#else
        return g_pCompositor->getWorkspacesCopy();
#endif
    }

    inline PHLWORKSPACE workspaceByID(WORKSPACEID id) {
#ifdef HS_HYPRLAND_056
        return State::workspaceState()->query().id(id).run();
#else
        return g_pCompositor->getWorkspaceByID(id);
#endif
    }

    // "a fullscreen window covers this monitor" — the hide_on_fullscreen gate.
    inline bool monitorHasFullscreen(const PHLMONITOR& mon) {
#ifdef HS_HYPRLAND_056
        return Fullscreen::controller()->hasFullscreen(mon);
#else
        return mon->inFullscreenMode();
#endif
    }

    // does any surface in `layers` claim `pos`? (overlay/top gating for input)
    template <typename Layers>
    inline bool layerSurfaceAt(const Vector2D& pos, Layers* layers, Vector2D* coords, PHLLS* ls) {
#ifdef HS_HYPRLAND_056
        return static_cast<bool>(Desktop::viewState()->hitTest().layerSurfaceAt(pos, layers, coords, ls));
#else
        return static_cast<bool>(g_pCompositor->vectorToLayerSurface(pos, layers, coords, ls));
#endif
    }
}
