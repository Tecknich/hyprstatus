#pragma once
// Hyprland API access, 0.57+.
//
// main targets Hyprland 0.57. Older releases are served by hyprpm.toml
// commit_pins (0.55.4, 0.56.2) at the last commit verified against them, so
// this header carries no pre-0.57 branches. Callers go through these Compat::
// wrappers so the next relocation is a one-file change again.
//
// 0.56 moved compositor-owned state into trackers (State::, Fullscreen::,
// Desktop::viewState()); 0.57 then threaded a Render::CRenderContext through
// all rendering, replaced CWorkspace with Workspace::CHLWorkspace (variant
// IDs), moved KeybindManager into Keybinds::, and dropped the hyprlang config
// provider.
#if !__has_include(<hyprland/src/workspace/HLWorkspace.hpp>)
#error "hyprstatus main requires Hyprland >= 0.57 headers. Older releases build the hyprpm.toml commit_pins entry."
#endif

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/render/Context.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>

// ">= 0.56 features present" (custom plugin bus events, specialActive, ...).
#define HS_HYPRLAND_056 1

namespace Compat {
    inline const std::vector<PHLMONITOR>& monitors() {
        return State::monitorState()->monitors();
    }

    inline std::vector<PHLWORKSPACE> workspacesCopy() {
        return State::workspaceState()->workspacesCopy();
    }

    // "a fullscreen window covers this monitor" — the hide_on_fullscreen gate.
    inline bool monitorHasFullscreen(const PHLMONITOR& mon) {
        return Fullscreen::controller()->hasFullscreen(mon);
    }

    // Re-queue the software cursor so it lands ABOVE everything queued so far.
    //
    // Hyprland renders the cursor and only THEN emits RENDER_LAST_MOMENT (0.57.2
    // Renderer.cpp: renderSoftwareCursorsFor() in the `renderCursor` block, the
    // stage emitted ~10 lines below it — unchanged since 0.56), and CRenderPass
    // draws elements in insertion order with no way to reorder. So anything a
    // plugin draws at that stage paints OVER the pointer; drawing the cursor a
    // second time keeps our overlays above the top/overlay layers while leaving
    // the pointer visible on top of them.
    //
    // No-op while a healthy hardware cursor plane is up: that plane composites
    // above the framebuffer anyway, and renderSoftwareCursorsFor() would then
    // only re-send a frame callback to the cursor surface. Otherwise the second
    // call is idempotent — one more textured quad, the same swRendered box. The
    // call mirrors Hyprland's own (ctx, mon, now, ctx.m_data.damage).
    inline void raiseSoftwareCursor(Render::CRenderContext& ctx, const PHLMONITOR& mon) {
        if (!mon || !Pointer::mgr() || Pointer::mgr()->hasVisibleHWCursor(mon))
            return;
        Pointer::mgr()->renderSoftwareCursorsFor(ctx, mon, Time::steadyNow(), ctx.m_data.damage);
    }

    // does any surface in `layers` claim `pos`? (overlay/top gating for input)
    template <typename Layers>
    inline bool layerSurfaceAt(const Vector2D& pos, Layers* layers, Vector2D* coords, PHLLS* ls) {
        return static_cast<bool>(Desktop::viewState()->hitTest().layerSurfaceAt(pos, layers, coords, ls));
    }
}
