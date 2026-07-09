#define WLR_USE_UNSTABLE
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "../util/Format.hpp"
#include "Factories.hpp"

namespace {

class CWorkspacesModule : public IModule {
  public:
    explicit CWorkspacesModule(const SModuleConfig& cfg) : IModule(cfg) {}

    void init() override {
        auto& EV = Event::bus()->m_events;
        m_listeners.push_back(EV.workspace.created.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.workspace.removed.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.workspace.active.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.workspace.moveToMonitor.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.window.moveToWorkspace.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.window.urgent.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.window.open.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.window.close.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.monitor.focused.listen([this] { requestRedraw(); }));
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

        for (const auto& WS : g_pCompositor->getWorkspacesCopy()) {
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
        dispatchWorkspace(std::to_string((int64_t)seg.id));
    }

    void onScroll(double delta, const SSegment&, PHLMONITOR) override {
        dispatchWorkspace(delta < 0 ? "-1" : "+1");
    }

  private:
    // deferred: switching workspaces from inside an input callback would
    // mutate compositor state mid-iteration
    static void dispatchWorkspace(const std::string& arg) {
        g_pEventLoopManager->doLater([arg] {
            if (!g_pKeybindManager)
                return;
            const auto IT = g_pKeybindManager->m_dispatchers.find("workspace");
            if (IT != g_pKeybindManager->m_dispatchers.end())
                IT->second(arg);
        });
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

    std::vector<CHyprSignalListener> m_listeners;
};

} // namespace

UP<IModule> makeWorkspacesModule(const SModuleConfig& cfg) {
    return makeUnique<CWorkspacesModule>(cfg);
}
