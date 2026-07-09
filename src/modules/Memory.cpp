#include "Factories.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
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
            std::ifstream f("/proc/meminfo");
            if (!f.is_open())
                return;

            unsigned long long memTotal = 0, memAvail = 0, swapTotal = 0, swapFree = 0;
            std::string        key;
            unsigned long long val   = 0;
            int                found = 0;
            while (found < 4 && f >> key >> val) {
                f.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                if (key == "MemTotal:") {
                    memTotal = val;
                    found++;
                } else if (key == "MemAvailable:") {
                    memAvail = val;
                    found++;
                } else if (key == "SwapTotal:") {
                    swapTotal = val;
                    found++;
                } else if (key == "SwapFree:") {
                    swapFree = val;
                    found++;
                }
            }
            if (memTotal == 0)
                return;

            const auto USEDKB  = memTotal >= memAvail ? memTotal - memAvail : 0;
            const auto PERC    = std::lround(100.0 * (double)USEDKB / (double)memTotal);
            const auto SWAPPCT = swapTotal > 0 ? std::lround(100.0 * (double)(swapTotal - swapFree) / (double)swapTotal) : 0L;

            const auto TEXT = Fmt::replaceTokens(opt("format", "{percentage}%"), {
                                                                                     {"percentage", std::to_string(PERC)},
                                                                                     {"used", gib(USEDKB)},
                                                                                     {"total", gib(memTotal)},
                                                                                     {"avail", gib(memAvail)},
                                                                                     {"swapPercentage", std::to_string(SWAPPCT)},
                                                                                 });
            if (TEXT == m_text)
                return;
            m_text = TEXT;
            requestRedraw();
        }

        std::vector<SSegment> segments(PHLMONITOR) override {
            return {SSegment{.text = m_text}};
        }

      private:
        static std::string gib(unsigned long long kb) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.1f", (double)kb / (1024.0 * 1024.0));
            return buf;
        }

        std::string                 m_text;
        std::optional<CModuleTimer> m_timer;
    };
}

UP<IModule> makeMemoryModule(const SModuleConfig& cfg) {
    return makeUnique<CMemoryModule>(cfg);
}
