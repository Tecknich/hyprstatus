#include "Factories.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>

#include "../util/Format.hpp"

namespace {
    class CCpuModule : public IModule {
      public:
        explicit CCpuModule(const SModuleConfig& cfg) : IModule(cfg) {}

        void init() override {
            update(); // primes the /proc/stat sample; first reading shows 0%
            m_timer.emplace(std::chrono::seconds(std::max<int64_t>(optInt("interval", 10), 1)), [this] { update(); });
        }

        void update() override {
            const auto USAGE = sampleUsage();

            double load[1] = {0.0};
            if (::getloadavg(load, 1) < 1)
                load[0] = 0.0;
            char loadBuf[32];
            std::snprintf(loadBuf, sizeof(loadBuf), "%.1f", load[0]);

            m_tokens        = {{"usage", std::to_string(USAGE)}, {"load", loadBuf}};
            const auto TEXT = Fmt::replaceTokens(opt("format", "{usage}%"), m_tokens);
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
            return Fmt::replaceTokens(opt("tooltip-format", "CPU {usage}%   load {load}"), m_tokens);
        }

      private:
        long sampleUsage() {
            std::ifstream f("/proc/stat");
            std::string   line;
            if (!f.is_open() || !std::getline(f, line))
                return m_lastUsage;

            // cpu user nice system idle iowait irq softirq steal ...
            std::istringstream iss(line);
            std::string        label;
            iss >> label;
            unsigned long long v = 0, total = 0, idle = 0;
            for (int i = 0; iss >> v; ++i) {
                total += v;
                if (i == 3 || i == 4) // idle + iowait
                    idle += v;
            }

            long usage = m_lastUsage;
            if (m_hasPrev && total > m_prevTotal) {
                const auto DTOTAL = total - m_prevTotal;
                const auto DIDLE  = idle >= m_prevIdle ? idle - m_prevIdle : 0;
                usage             = std::lround(100.0 * (1.0 - (double)DIDLE / (double)DTOTAL));
                usage             = std::clamp(usage, 0L, 100L);
            }
            m_prevTotal = total;
            m_prevIdle  = idle;
            m_hasPrev   = true;
            m_lastUsage = usage;
            return usage;
        }

        unsigned long long          m_prevTotal = 0, m_prevIdle = 0;
        bool                        m_hasPrev   = false;
        long                                 m_lastUsage = 0;
        std::string                          m_text;
        std::map<std::string, std::string>   m_tokens;
        std::optional<CModuleTimer>          m_timer;
    };
}

UP<IModule> makeCpuModule(const SModuleConfig& cfg) {
    return makeUnique<CCpuModule>(cfg);
}
