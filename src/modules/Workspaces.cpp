#define WLR_USE_UNSTABLE
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/state/workspace/Resolver.hpp>
#include <hyprland/src/state/workspace/State.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/workspace/WorkspaceUtils.hpp>

#include <algorithm>
#include <charconv>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "../util/Format.hpp"
#include "../util/HyprCompat.hpp"
#include "Factories.hpp"

namespace {

class CWorkspacesModule : public IModule {
  public:
    explicit CWorkspacesModule(const SModuleConfig& cfg) : IModule(cfg) {}

    void init() override {
        auto& EV = Event::bus()->m_events;
        // createdLate: the workspace is fully set up (monitor, name) by then
        m_listeners.push_back(EV.workspace.createdLate.listen([this](const PHLWORKSPACEREF& ws) {
            watchWorkspace(ws.lock());
            requestRedraw();
        }));
        m_listeners.push_back(EV.workspace.removed.listen([this](const PHLWORKSPACEREF&) {
            // emitted mid-destruction, so the ref may already be unlockable:
            // sweep every expired watch rather than looking this one up
            std::erase_if(m_wsWatches, [](const SWorkspaceWatch& W) { return W.ws.expired(); });
            requestRedraw();
        }));
        // 0.57 finally has bus events for renames and special open/close/steal
        // (the latter repaints both monitors: requestRedraw damages every bar
        // and segments() reads m_activeSpecialWorkspace live)
        m_listeners.push_back(EV.workspace.renamed.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.workspace.active.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.workspace.specialActive.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.workspace.moveToMonitor.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.window.moveToWorkspace.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.window.urgent.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.window.open.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.window.close.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.monitor.focused.listen([this] { requestRedraw(); }));

        for (const auto& WS : Compat::workspacesCopy())
            watchWorkspace(WS);
    }

    std::vector<SSegment> segments(PHLMONITOR mon) override {
        if (!mon)
            return {};

        const auto FORMAT        = opt("format", "{name}");
        const bool SHOWSPECIAL   = optBool("show-special", false);
        const auto ACTIVE        = mon->m_activeWorkspace;
        const auto ACTIVESPECIAL = mon->m_activeSpecialWorkspace; // null = none open here

        // Workspace IDs are no longer signed integers (0.57): a workspace is
        // numbered (uint32), named (type NORMAL but NO number — createNamed uses
        // the special-ID variant), or special. Branch on type(), never on
        // whether a number is present, or named workspaces get treated as special.
        struct SEntry {
            PHLWORKSPACE            ws; // null for a persistent id with no live workspace
            std::optional<uint32_t> num;
            std::string             label; // {name}
            std::string             ref;   // click target, see dispatchFor()
            bool                    special = false;
            bool                    urgent  = false;
        };
        std::vector<SEntry> entries;

        for (const auto& WS : Compat::workspacesCopy()) {
            if (!WS || WS->monitorID() != mon->m_id)
                continue;
            const bool SPECIAL = WS->type() == Workspace::eWorkspaceType::SPECIAL;
            // an ACTIVE special is always shown, even with show-special=false:
            // the whole point of the indicator is seeing that one is open
            if (SPECIAL && !SHOWSPECIAL && WS != ACTIVESPECIAL)
                continue;

            SEntry e{.ws = WS, .num = WS->numberedID(), .special = SPECIAL, .urgent = WS->hasUrgentWindow()};
            if (SPECIAL) {
                // addressableName is always "special:<n>" ("special:special" for
                // the unnamed one); show it as before, toggle by the bare name
                e.label = WS->addressableName();
                e.ref   = "s:" + e.label.substr(e.label.starts_with("special:") ? 8 : 0);
            } else if (e.num) {
                e.label = WS->displayName();
                e.ref   = "n:" + std::to_string(*e.num);
            } else {
                e.label = WS->displayName();
                e.ref   = "w:" + WS->addressableName();
            }
            entries.push_back(std::move(e));
        }

        for (const auto ID : persistentIDs(mon)) {
            if (std::ranges::any_of(entries, [ID](const SEntry& e) { return e.num == ID; }))
                continue;
            entries.push_back({.ws = nullptr, .num = ID, .label = std::to_string(ID), .ref = "n:" + std::to_string(ID)});
        }

        // specials first (where their negative ids used to sort them), then
        // numbered ascending, then named workspaces last
        std::ranges::stable_sort(entries, [](const SEntry& a, const SEntry& b) {
            const auto RANK = [](const SEntry& e) { return e.special ? 0 : e.num ? 1 : 2; };
            if (RANK(a) != RANK(b))
                return RANK(a) < RANK(b);
            if (a.num && b.num)
                return *a.num < *b.num;
            // named / special: keep workspacesCopy() order (creation order). The
            // old module sorted them by their creation-time negative id, so a
            // rename never moved a button; comparing labels would.
            return false;
        });

        std::vector<SSegment> out;
        out.reserve(entries.size());
        for (const auto& E : entries) {
            SSegment seg;
            seg.text      = Fmt::replaceTokens(FORMAT, {{"id", E.num ? std::to_string(*E.num) : E.label}, {"name", E.label}});
            seg.hoverable = true;
            seg.ref       = E.ref;
            if (E.special && E.ws && E.ws == ACTIVESPECIAL)
                seg.cls = "special-active";
            else if (E.ws && E.ws == ACTIVE)
                seg.cls = "active";
            else if (E.urgent)
                seg.cls = "urgent";
            else if (!E.ws)
                seg.cls = "persistent";
            out.push_back(std::move(seg));
        }
        return out;
    }

    void onClick(uint32_t /*button*/, const SSegment& seg, PHLMONITOR) override {
        // Absolute switch to the CLICKED workspace. A persistent id with no live
        // workspace yet is created exactly as SUPER+N would create it.
        const auto& R = seg.ref;
        if (R.size() < 3 || R[1] != ':')
            return;
        const auto ARG = R.substr(2);
        if (R[0] == 'n') {
            uint32_t   n = 0;
            const auto [PTR, EC] = std::from_chars(ARG.data(), ARG.data() + ARG.size(), n);
            if (EC == std::errc{} && PTR == ARG.data() + ARG.size() && n > 0)
                switchTo(std::to_string(n));
        } else if (R[0] == 'w')
            switchTo("name:" + ARG);
        else if (R[0] == 's')
            toggleSpecial(ARG);
    }

    void onScroll(double delta, const SSegment&, PHLMONITOR) override {
        // Relative step, as before: "+1"/"-1" is the active workspace's NUMBER
        // plus one. 0.57 named workspaces have no number, and the resolver then
        // rejects a plain relative selector ("Relative workspace on no numbered
        // workspace!"), so scrolling the bar on a named workspace silently did
        // nothing. Fall back to "r+1" there — Hyprland's open-workspace walk,
        // which works from any workspace. Numbered workspaces keep exact 0.56
        // behaviour ("r" also skips workspaces held by other monitors, which
        // would change multi-monitor scrolling). Checked against the focused
        // monitor because that is the resolver's default base.
        const auto FOCUSED  = Desktop::focusState()->monitor();
        const bool NUMBERED = FOCUSED && FOCUSED->m_activeWorkspace && FOCUSED->m_activeWorkspace->numberedID();
        const auto STEP     = std::string{NUMBERED ? "" : "r"} + (delta < 0 ? "-1" : "+1");
        switchTo(STEP);
    }

  private:
    // Deferred: invoking a dispatcher from inside an input callback would
    // mutate compositor state mid-iteration. Use doLaterLock (RAII) rather
    // than bare doLater so the pending idle is cancelled when this module is
    // destroyed at PLUGIN_EXIT -- otherwise it would run unmapped .so code
    // after dlclose and crash the session. One pending dispatch suffices;
    // reassigning m_dispatchLater cancels any previous one (last click wins).
    //
    // 0.57 has no C++ dispatcher table, but the Lua dispatchers are thin
    // wrappers over Config::Actions: hl.dsp.focus({ workspace = S }) IS
    // Config::Actions::changeWorkspace(S). Call that directly. Going through
    // `hyprctl dispatch` instead runs a Lua eval, and CConfigManager::eval clears
    // the stored config errors -- every bar click would silently empty
    // `hyprctl configerrors` while the config was still broken.
    void switchTo(std::string selector) {
        m_dispatchLater = g_pEventLoopManager->doLaterLock([SEL = std::move(selector)] {
            if (const auto RES = Config::Actions::changeWorkspace(SEL); !RES)
                LOG(Log::ERR, "[hyprstatus] workspaces: switch to `{}` failed: {}", SEL, RES.error().message);
        });
    }

    // Same steps as Hyprland's own hl.dsp.workspace.toggle_special (v0.57.2
    // LuaBindingsDispatchers.cpp dsp_toggleSpecial): resolve the special by its
    // bare name, create it on the focused monitor if it does not exist yet,
    // toggle, then ready() the workspace.
    void toggleSpecial(std::string name) {
        m_dispatchLater = g_pEventLoopManager->doLaterLock([NAME = std::move(name)] {
            const auto TARGET = State::Workspace::resolver()->getWorkspaceTargetFromString(Workspace::specialWorkspaceAddressFromName(NAME));
            if (!TARGET.valid() || TARGET.type != Workspace::eWorkspaceType::SPECIAL) {
                LOG(Log::ERR, "[hyprstatus] workspaces: `{}` is not a valid special workspace", NAME);
                return;
            }
            auto ws = State::Workspace::state()->find(TARGET);
            if (!ws) {
                if (const auto MON = Desktop::focusState()->monitor())
                    ws = State::Workspace::state()->create(TARGET, MON);
            }
            if (!ws)
                return;
            if (const auto RES = Config::Actions::toggleSpecial(ws); !RES)
                LOG(Log::ERR, "[hyprstatus] workspaces: toggling special `{}` failed: {}", NAME, RES.error().message);
            ws->ready();
        });
    }

    // Per-object CHLWorkspace signals with no bus equivalent: idChanged
    // (hl.dsp.workspace.change_id relabels a numbered workspace in place).
    void watchWorkspace(const PHLWORKSPACE& ws) {
        if (!ws)
            return;
        if (std::ranges::any_of(m_wsWatches, [&ws](const SWorkspaceWatch& W) { return W.ws.lock() == ws; }))
            return;
        m_wsWatches.push_back({.ws = ws, .idChanged = ws->m_events.idChanged.listen([this] { requestRedraw(); })});
    }

    // positive integers only: numbered workspaces are uint32 and 0 is invalid
    std::vector<uint32_t> persistentIDs(const PHLMONITOR& mon) const {
        auto list = opt("persistent");
        if (const auto OVERRIDE = opt("persistent." + mon->m_name); !OVERRIDE.empty())
            list = OVERRIDE;

        std::vector<uint32_t> ids;
        for (const auto& TOK : Fmt::split(list, ' ')) {
            const auto T = Fmt::trim(TOK);
            if (const auto V = Fmt::toLL(T); V && *V > 0 && *V <= (long long)std::numeric_limits<uint32_t>::max())
                ids.push_back((uint32_t)*V);
        }
        return ids;
    }

    struct SWorkspaceWatch {
        PHLWORKSPACEREF     ws; // identity + liveness: expired => swept on workspace.removed
        CHyprSignalListener idChanged;
    };

    std::vector<CHyprSignalListener> m_listeners;
    std::vector<SWorkspaceWatch>     m_wsWatches;
    UP<SEventLoopDoLaterLock>        m_dispatchLater; // RAII-cancelled pending dispatch
};

} // namespace

UP<IModule> makeWorkspacesModule(const SModuleConfig& cfg) {
    return makeUnique<CWorkspacesModule>(cfg);
}
