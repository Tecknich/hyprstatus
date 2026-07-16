#include "Factories.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <string>

#include "../util/Format.hpp"

namespace {
    class CMemoryModule : public IModule {
      public:
        explicit CMemoryModule(const SModuleConfig& cfg) : IModule(cfg) {}

        void init() override {
            update();
            m_timer.emplace(std::chrono::seconds(std::max<int64_t>(optInt("interval", 30), 1)), [this] { update(); });
        }

        void update() override {
            const auto CONTENT = Fmt::readFile("/proc/meminfo");
            if (CONTENT.empty())
                return;

            unsigned long long memTotal = 0, memAvail = 0, swapTotal = 0, swapFree = 0;
            int                found = 0;
            size_t             pos   = 0;
            while (found < 4 && pos < CONTENT.size()) {
                const auto EOL  = CONTENT.find('\n', pos);
                const auto LINE = CONTENT.substr(pos, EOL == std::string::npos ? std::string::npos : EOL - pos);
                pos             = EOL == std::string::npos ? CONTENT.size() : EOL + 1;

                const auto TOKS = Fmt::tokens(LINE);
                if (TOKS.size() < 2)
                    continue;
                const auto VAL = Fmt::toULL(TOKS[1]).value_or(0);
                if (TOKS[0] == "MemTotal:") {
                    memTotal = VAL;
                    found++;
                } else if (TOKS[0] == "MemAvailable:") {
                    memAvail = VAL;
                    found++;
                } else if (TOKS[0] == "SwapTotal:") {
                    swapTotal = VAL;
                    found++;
                } else if (TOKS[0] == "SwapFree:") {
                    swapFree = VAL;
                    found++;
                }
            }
            if (memTotal == 0)
                return;

            const auto USEDKB  = memTotal >= memAvail ? memTotal - memAvail : 0;
            const auto PERC    = std::lround(100.0 * (double)USEDKB / (double)memTotal);
            const auto SWAPPCT = swapTotal > 0 ? std::lround(100.0 * (double)(swapTotal - swapFree) / (double)swapTotal) : 0L;

            m_tokens        = {{"percentage", std::to_string(PERC)}, {"used", gib(USEDKB)}, {"total", gib(memTotal)},
                               {"avail", gib(memAvail)}, {"swapPercentage", std::to_string(SWAPPCT)}};
            const auto TEXT = Fmt::replaceTokens(opt("format", "{percentage}%"), m_tokens);
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
            if (!seg.tooltip.empty())
                return seg.tooltip;
            return Fmt::replaceTokens(opt("tooltip-format", "{used} / {total} GiB used ({percentage}%)\n{avail} GiB available"), m_tokens);
        }

      private:
        static std::string gib(unsigned long long kb) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.1f", (double)kb / (1024.0 * 1024.0));
            return buf;
        }

        std::string                        m_text;
        std::map<std::string, std::string> m_tokens;
        std::optional<CModuleTimer>        m_timer;
    };
}

UP<IModule> makeMemoryModule(const SModuleConfig& cfg) {
    return makeUnique<CMemoryModule>(cfg);
}
