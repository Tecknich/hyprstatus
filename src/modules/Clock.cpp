#include "Factories.hpp"

#include <chrono>
#include <ctime>
#include <optional>
#include <string>

#include "../util/Format.hpp"

namespace {
    class CClockModule : public IModule {
      public:
        explicit CClockModule(const SModuleConfig& cfg) : IModule(cfg) {}

        void init() override {
            m_intervalMs = std::max<int64_t>(optInt("interval", 60), 1) * 1000;
            update();
            m_timer.emplace(msToNextBoundary(), [this] {
                update();
                // re-align: the self-rearm picks this up (setInterval applies on re-arm)
                m_timer->setInterval(msToNextBoundary());
            });
        }

        void update() override {
            const auto TEXT = Fmt::strftimeFmt(opt("format", "%H:%M"), ::time(nullptr));
            if (TEXT == m_text)
                return;
            m_text = TEXT;
            requestRedraw();
        }

        std::vector<SSegment> segments(PHLMONITOR) override {
            return {SSegment{.text = m_text}};
        }

        std::string tooltip(const SSegment& seg) override {
            if (!optBool("tooltip", true))
                return "";
            auto fmt = !seg.tooltip.empty() ? seg.tooltip : opt("tooltip-format", "{calendar}");

            static const std::string TOKEN = "{calendar}";
            if (fmt.find(TOKEN) != std::string::npos) {
                const auto CAL = Fmt::calendarGrid(::time(nullptr));
                size_t     pos = 0;
                while ((pos = fmt.find(TOKEN, pos)) != std::string::npos) {
                    fmt.replace(pos, TOKEN.size(), CAL);
                    pos += CAL.size();
                }
            }
            return Fmt::stripPango(fmt);
        }

      private:
        std::chrono::milliseconds msToNextBoundary() const {
            using namespace std::chrono;
            const auto NOWMS = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
            auto       delta = m_intervalMs - (NOWMS % m_intervalMs);
            if (delta < 20) // fired marginally early/on the boundary: skip to the next one
                delta += m_intervalMs;
            return milliseconds(delta + 5); // small cushion so the tick lands past the boundary
        }

        int64_t                     m_intervalMs = 60000;
        std::string                 m_text;
        std::optional<CModuleTimer> m_timer;
    };
}

UP<IModule> makeClockModule(const SModuleConfig& cfg) {
    return makeUnique<CClockModule>(cfg);
}
