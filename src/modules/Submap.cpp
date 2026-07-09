#define WLR_USE_UNSTABLE
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>

#include <string>
#include <vector>

#include "../util/Format.hpp"
#include "Factories.hpp"

namespace {

class CSubmapModule : public IModule {
  public:
    explicit CSubmapModule(const SModuleConfig& cfg) : IModule(cfg) {}

    void init() override {
        readCurrentSubmap();
        // payload is the submap name; "" on reset back to the default map
        m_listener = Event::bus()->m_events.keybinds.submap.listen([this](const std::string& submap) {
            m_submap = submap;
            requestRedraw();
        });
    }

    void update() override {
        readCurrentSubmap();
        requestRedraw();
    }

    std::vector<SSegment> segments(PHLMONITOR) override {
        if (m_submap.empty())
            return {};
        SSegment seg;
        seg.text = Fmt::replaceTokens(opt("format", "{}"), {{"", m_submap}});
        return {seg};
    }

    bool hidden(PHLMONITOR) override {
        return m_submap.empty();
    }

  private:
    void readCurrentSubmap() {
        if (g_pKeybindManager)
            m_submap = g_pKeybindManager->getCurrentSubmap().name;
    }

    std::string         m_submap;
    CHyprSignalListener m_listener;
};

} // namespace

UP<IModule> makeSubmapModule(const SModuleConfig& cfg) {
    return makeUnique<CSubmapModule>(cfg);
}
