#define WLR_USE_UNSTABLE
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/Monitor.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "../util/Format.hpp"
#include "Factories.hpp"

namespace {

class CWindowModule : public IModule {
  public:
    explicit CWindowModule(const SModuleConfig& cfg) : IModule(cfg) {}

    void init() override {
        auto& EV = Event::bus()->m_events;
        m_listeners.push_back(EV.window.title.listen([this] { requestRedraw(); }));
        // window.active payload PHLWINDOW may be NULL (focus loss); we only
        // need "focus changed" so the args are ignored entirely
        m_listeners.push_back(EV.window.active.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.window.close.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.window.destroy.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.workspace.active.listen([this] { requestRedraw(); }));
        m_listeners.push_back(EV.monitor.focused.listen([this] { requestRedraw(); }));
    }

    std::vector<SSegment> segments(PHLMONITOR mon) override {
        const auto TEXT = textFor(mon);
        if (TEXT.empty())
            return {};
        SSegment seg;
        seg.text = TEXT;
        return {seg};
    }

    bool hidden(PHLMONITOR mon) override {
        return textFor(mon).empty();
    }

  private:
    PHLWINDOW windowFor(const PHLMONITOR& mon) const {
        if (!mon)
            return nullptr;
        const auto FS = Desktop::focusState();
        if (FS && FS->monitor() == mon)
            return FS->window();
        return mon->m_activeWorkspace ? mon->m_activeWorkspace->getLastFocusedWindow() : nullptr;
    }

    std::string textFor(const PHLMONITOR& mon) const {
        const auto W = windowFor(mon);
        if (!Desktop::View::validMapped(W))
            return "";
        const auto TEXT = Fmt::replaceTokens(opt("format", "{title}"), {{"title", W->m_title}, {"class", W->m_class}});
        return Fmt::truncate(TEXT, (size_t)std::max((int64_t)0, optInt("max-length", 80)));
    }

    std::vector<CHyprSignalListener> m_listeners;
};

} // namespace

UP<IModule> makeWindowModule(const SModuleConfig& cfg) {
    return makeUnique<CWindowModule>(cfg);
}
