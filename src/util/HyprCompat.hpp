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

// CMonitor's definition is needed by monitorHasFullscreen below; the header
// moved in 0.56 (helpers/Monitor.hpp -> output/Monitor.hpp).
#if __has_include(<hyprland/src/output/Monitor.hpp>)
#include <hyprland/src/output/Monitor.hpp>
#else
#include <hyprland/src/helpers/Monitor.hpp>
#endif

#if __has_include(<hyprland/src/state/MonitorState.hpp>)
#define HS_HYPRLAND_056 1
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#endif

// The pointer manager moved to pointer/PointerManager.hpp (namespace Pointer) in
// 0.56; raiseSoftwareCursor() below needs it, plus the render pass and clock.
#if __has_include(<hyprland/src/pointer/PointerManager.hpp>)
#define HS_HAS_POINTER_MGR 1
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/render/Renderer.hpp>
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

    // Re-queue the software cursor so it lands ABOVE everything queued so far.
    //
    // Hyprland renders the cursor and only THEN emits RENDER_LAST_MOMENT (0.56.2
    // Renderer.cpp: renderSoftwareCursorsFor() in the `renderCursor` block, the
    // stage emitted ~10 lines below it), and CRenderPass draws elements in
    // insertion order with no way to reorder — m_passElements is private and the
    // public API is only add()/clear()/removeAllOfType(). So anything a plugin
    // draws at that stage necessarily paints OVER the pointer. Drawing the cursor
    // a second time is what keeps our overlays above the top/overlay layers while
    // still leaving the pointer visible on top of them.
    //
    // No-op while a healthy hardware cursor plane is up: that plane composites
    // above the whole framebuffer anyway, and renderSoftwareCursorsFor() would
    // then only re-send a frame callback to the cursor surface. Second call is
    // otherwise idempotent — it re-adds one textured quad and re-stamps the same
    // swRendered box.
    inline void raiseSoftwareCursor(const PHLMONITOR& mon) {
#ifdef HS_HAS_POINTER_MGR
        if (!mon || !Pointer::mgr() || Pointer::mgr()->hasVisibleHWCursor(mon))
            return;
        Pointer::mgr()->renderSoftwareCursorsFor(mon, Time::steadyNow(), g_pHyprRenderer->m_renderData.damage);
#else
        (void)mon; // 0.55: pointer manager lived elsewhere; those users are commit-pinned
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
