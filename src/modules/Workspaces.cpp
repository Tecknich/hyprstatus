#define WLR_USE_UNSTABLE
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/event/EventBus.hpp>
// 0.56 moved CMonitor: helpers/Monitor.hpp -> output/Monitor.hpp
#if __has_include(<hyprland/src/output/Monitor.hpp>)
#include <hyprland/src/output/Monitor.hpp>
#else
#include <hyprland/src/helpers/Monitor.hpp>
#endif
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>

#include <algorithm>
#include <string>
#include <unordered_map>
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
        m_listeners.push_back(EV.workspace.created.listen([this](const PHLWORKSPACEREF& ws) {
            watchRename(ws.lock());
            requestRedraw();
        }));
        m_listeners.push_back(EV.workspace.removed.listen([this](const PHLWORKSPACEREF& ws) {
            if (const auto WS = ws.lock())
                m_renameListeners.erase(WS->m_id);
            requestRedraw();
        }));
        m_listeners.push_back(EV.workspace.active.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.workspace.moveToMonitor.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.window.moveToWorkspace.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.window.urgent.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.window.open.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.window.close.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.monitor.focused.listen([this] { requestRedraw(); }));

        // Workspace renames have NO bus event; only the per-object
        // CWorkspace::m_events.renamed signal fires. Watch every live
        // workspace now, and each newly created one above, so the bar
        // redraws after `hyprctl dispatch renameworkspace ...`.
        for (const auto& WS : Compat::workspacesCopy())
            watchRename(WS);
    }

    std::vector<SSegment> segments(PHLMONITOR mon) override {
        if (!mon)
            return {};

        const auto FORMAT      = opt("format", "{name}");
        const bool SHOWSPECIAL = optBool("show-special", false);

        struct SEntry {
            WORKSPACEID id      = 0;
            std::string name;
            bool        special = false;
            bool        urgent  = false;
            bool        virt    = false; // persistent id with no live workspace
        };
        std::vector<SEntry> entries;

        for (const auto& WS : Compat::workspacesCopy()) {
            if (!WS || WS->inert())
                continue;
            if (WS->monitorID() != mon->m_id)
                continue;
            if (WS->m_isSpecialWorkspace && !SHOWSPECIAL)
                continue;
            entries.push_back({WS->m_id, WS->m_name, WS->m_isSpecialWorkspace, WS->hasUrgentWindow(), false});
        }

        for (const auto ID : persistentIDs(mon)) {
            if (std::ranges::any_of(entries, [ID](const SEntry& e) { return e.id == ID; }))
                continue;
            entries.push_back({ID, std::to_string(ID), false, false, true});
        }

        // ascending by id, named workspaces (id < 0, non-special) last
        std::ranges::sort(entries, [](const SEntry& a, const SEntry& b) {
            const bool ANAMED = a.id < 0 && !a.special;
            const bool BNAMED = b.id < 0 && !b.special;
            if (ANAMED != BNAMED)
                return BNAMED;
            return a.id < b.id;
        });

        const auto ACTIVEID = mon->activeWorkspaceID();

        std::vector<SSegment> out;
        out.reserve(entries.size());
        for (const auto& E : entries) {
            SSegment seg;
            seg.text      = Fmt::replaceTokens(FORMAT, {{"id", std::to_string(E.id)}, {"name", E.name}});
            seg.hoverable = true;
            seg.id        = (size_t)(int64_t)E.id;
            if (E.id == ACTIVEID)
                seg.cls = "active";
            else if (E.urgent)
                seg.cls = "urgent";
            else if (E.virt)
                seg.cls = "persistent";
            out.push_back(std::move(seg));
        }
        return out;
    }

    void onClick(uint32_t /*button*/, const SSegment& seg, PHLMONITOR) override {
        // Absolute switch to the CLICKED workspace. seg.id carries the signed
        // workspace id (round-tripped through size_t). A bare negative number
        // is parsed as a RELATIVE jump by the `workspace` dispatcher, so named
        // (id < 0) and special workspaces must be dispatched by name.
        const WORKSPACEID ID = (int64_t)seg.id;
        if (const auto WS = Compat::workspaceByID(ID); WS && !WS->inert()) {
            if (WS->m_isSpecialWorkspace) {
                // togglespecialworkspace prepends "special:"; m_name is
                // "special:<n>" (or "special:special" for the default).
                std::string name = WS->m_name;
                if (name.starts_with("special:"))
                    name = name.substr(8);
                dispatchWorkspace("togglespecialworkspace", name);
                return;
            }
            if (ID < 0) {
                dispatchWorkspace("workspace", "name:" + WS->m_name);
                return;
            }
        }
        // live numbered workspace or persistent (not-yet-created) positive id
        dispatchWorkspace("workspace", std::to_string(ID));
    }

    void onScroll(double delta, const SSegment&, PHLMONITOR) override {
        // relative step is the intended scroll behavior
        dispatchWorkspace("workspace", delta < 0 ? "-1" : "+1");
    }

  private:
    // Deferred: invoking a dispatcher from inside an input callback would
    // mutate compositor state mid-iteration. Use doLaterLock (RAII) rather
    // than bare doLater so the pending idle is cancelled when this module is
    // destroyed at PLUGIN_EXIT -- otherwise it would run unmapped .so code
    // after dlclose and crash the session. One pending dispatch suffices;
    // reassigning m_dispatchLater cancels any previous one (last click wins).
    void dispatchWorkspace(const std::string& dispatcher, const std::string& arg) {
        m_dispatchLater = g_pEventLoopManager->doLaterLock([dispatcher, arg] {
            if (!g_pKeybindManager)
                return;
            const auto IT = g_pKeybindManager->m_dispatchers.find(dispatcher);
            if (IT != g_pKeybindManager->m_dispatchers.end())
                IT->second(arg);
        });
    }

    // Subscribe to a workspace's per-object renamed signal (no bus event
    // exists for renames). Idempotent per workspace id.
    void watchRename(const PHLWORKSPACE& ws) {
        if (!ws || ws->inert())
            return;
        if (m_renameListeners.contains(ws->m_id))
            return;
        m_renameListeners.emplace(ws->m_id, ws->m_events.renamed.listen([this] { requestRedraw(); }));
    }

    std::vector<WORKSPACEID> persistentIDs(const PHLMONITOR& mon) const {
        auto list = opt("persistent");
        if (const auto OVERRIDE = opt("persistent." + mon->m_name); !OVERRIDE.empty())
            list = OVERRIDE;

        std::vector<WORKSPACEID> ids;
        for (const auto& TOK : Fmt::split(list, ' ')) {
            const auto T = Fmt::trim(TOK);
            if (T.empty())
                continue;
            try {
                ids.push_back(std::stoll(T));
            } catch (...) {}
        }
        return ids;
    }

    std::vector<CHyprSignalListener>                        m_listeners;
    std::unordered_map<WORKSPACEID, CHyprSignalListener>    m_renameListeners; // per-workspace renamed hooks
    UP<SEventLoopDoLaterLock>                               m_dispatchLater;   // RAII-cancelled pending dispatch
};

} // namespace

UP<IModule> makeWorkspacesModule(const SModuleConfig& cfg) {
    return makeUnique<CWorkspacesModule>(cfg);
}
