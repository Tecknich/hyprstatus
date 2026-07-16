#include "Factories.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

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
            const auto USAGE = sample(); // total + per-core in one /proc/stat pass

            double load[1] = {0.0};
            if (::getloadavg(load, 1) < 1)
                load[0] = 0.0;
            char loadBuf[32];
            std::snprintf(loadBuf, sizeof(loadBuf), "%.1f", load[0]);
            m_load = loadBuf;

            m_tokens = {{"usage", std::to_string(USAGE)}, {"load", m_load}};
            // expose per-core percentages as {core0}{core1}... for custom formats
            for (const auto& [idx, c] : m_cores)
                m_tokens["core" + std::to_string(idx)] = std::to_string(c.lastPct);

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
            // user-supplied format wins; otherwise the per-core grid is the default
            if (hasOpt("tooltip-format"))
                return Fmt::replaceTokens(opt("tooltip-format"), m_tokens);
            return defaultTooltip();
        }

      private:
        struct SCore {
            unsigned long long prevTotal = 0, prevIdle = 0;
            bool               hasPrev = false;
            long               lastPct = 0;
        };

        // Parse one already-label-stripped /proc/stat cpu line (numeric fields
        // only) and compute its usage% across the interval, updating the
        // caller's previous sample.
        static long computeUsage(const std::vector<std::string>& fields, unsigned long long& prevTotal, unsigned long long& prevIdle, bool& hasPrev, long lastPct) {
            unsigned long long total = 0, idle = 0;
            for (size_t i = 0; i < fields.size(); ++i) {
                const auto V = Fmt::toULL(fields[i]);
                if (!V)
                    break;
                total += *V;
                if (i == 3 || i == 4) // idle + iowait
                    idle += *V;
            }

            long usage = lastPct;
            if (hasPrev && total > prevTotal) {
                const auto DTOTAL = total - prevTotal;
                const auto DIDLE  = idle >= prevIdle ? idle - prevIdle : 0;
                usage             = std::lround(100.0 * (1.0 - (double)DIDLE / (double)DTOTAL));
                usage             = std::clamp(usage, 0L, 100L);
            }
            prevTotal = total;
            prevIdle  = idle;
            hasPrev   = true;
            return usage;
        }

        // Reads the aggregate "cpu" line plus every "cpuN" per-core line. On read
        // failure, keeps the last good sample. Returns aggregate usage%.
        long sample() {
            const auto CONTENT = Fmt::readFile("/proc/stat");
            if (CONTENT.empty())
                return m_lastUsage; // keep last good

            std::set<int> seen;
            bool          gotAgg = false;
            size_t        pos    = 0;

            while (pos < CONTENT.size()) {
                const auto EOL  = CONTENT.find('\n', pos);
                const auto LINE = CONTENT.substr(pos, EOL == std::string::npos ? std::string::npos : EOL - pos);
                pos             = EOL == std::string::npos ? CONTENT.size() : EOL + 1;

                auto toks = Fmt::tokens(LINE);
                if (toks.empty())
                    continue;
                const std::string LABEL = toks[0];
                if (LABEL.compare(0, 3, "cpu") != 0)
                    break; // past the cpu block (intr, ctxt, ...)
                toks.erase(toks.begin());

                if (LABEL == "cpu") {
                    m_lastUsage = computeUsage(toks, m_prevTotal, m_prevIdle, m_hasPrev, m_lastUsage);
                    gotAgg      = true;
                    continue;
                }

                // per-core line: ^cpu[0-9]+
                const std::string DIGITS = LABEL.substr(3);
                if (DIGITS.empty() || !std::all_of(DIGITS.begin(), DIGITS.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; }))
                    continue;
                const auto IDX = Fmt::toLL(DIGITS);
                if (!IDX || *IDX < 0 || *IDX > 4096)
                    continue;
                const int idx = (int)*IDX;

                auto& c   = m_cores[idx];
                c.lastPct = computeUsage(toks, c.prevTotal, c.prevIdle, c.hasPrev, c.lastPct);
                seen.insert(idx);
            }

            // drop cores that went offline so the tooltip tracks live cores; a
            // returning core re-primes cleanly (fresh entry, first read = 0%)
            if (gotAgg) {
                for (auto it = m_cores.begin(); it != m_cores.end();) {
                    if (!seen.contains(it->first))
                        it = m_cores.erase(it);
                    else
                        ++it;
                }
            }

            return m_lastUsage;
        }

        // Default hover view: a vertical list, aggregate on top then one line
        // per core in ascending index order (Waybar-style). No trailing newline.
        std::string defaultTooltip() const {
            std::string out = "Total: " + std::to_string(m_lastUsage) + "%";
            for (const auto& [idx, c] : m_cores) {
                out += "\nCore" + std::to_string(idx) + ": " + std::to_string(c.lastPct) + "%";
            }
            return out;
        }

        unsigned long long                 m_prevTotal = 0, m_prevIdle = 0;
        bool                               m_hasPrev   = false;
        long                               m_lastUsage = 0;
        std::string                        m_text;
        std::string                        m_load;
        std::map<int, SCore>               m_cores; // keyed by core index (sorted)
        std::map<std::string, std::string> m_tokens;
        std::optional<CModuleTimer>        m_timer;
    };
}

UP<IModule> makeCpuModule(const SModuleConfig& cfg) {
    return makeUnique<CCpuModule>(cfg);
}
