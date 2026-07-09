#define WLR_USE_UNSTABLE
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/SeatManager.hpp>

#include <cctype>
#include <string>
#include <vector>

#include "../util/Format.hpp"
#include "Factories.hpp"

namespace {

class CLanguageModule : public IModule {
  public:
    explicit CLanguageModule(const SModuleConfig& cfg) : IModule(cfg) {}

    void init() override {
        readActiveLayout();
        m_listener = Event::bus()->m_events.input.keyboard.layout.listen(
            [this](const SP<IKeyboard>&, const std::string& layout) {
                m_layout = layout;
                requestRedraw();
            });
    }

    void update() override {
        readActiveLayout();
        requestRedraw();
    }

    std::vector<SSegment> segments(PHLMONITOR) override {
        if (m_layout.empty())
            return {};
        SSegment seg;
        seg.text = Fmt::replaceTokens(opt("format", "{}"), {{"", m_layout}, {"short", shortName()}});
        return {seg};
    }

    // no keyboard (or no known layout) -> hidden
    bool hidden(PHLMONITOR) override {
        return m_layout.empty();
    }

  private:
    void readActiveLayout() {
        if (!g_pSeatManager)
            return;
        if (const auto KB = g_pSeatManager->m_keyboard.lock())
            m_layout = KB->getActiveLayout();
        else
            m_layout.clear();
    }

    // "English (US)" -> "EN"
    std::string shortName() const {
        auto s = m_layout.substr(0, 2);
        for (auto& c : s)
            c = (char)std::toupper((unsigned char)c);
        return s;
    }

    std::string         m_layout;
    CHyprSignalListener m_listener;
};

} // namespace

UP<IModule> makeLanguageModule(const SModuleConfig& cfg) {
    return makeUnique<CLanguageModule>(cfg);
}
