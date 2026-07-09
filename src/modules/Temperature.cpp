#include "Factories.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>

#include "../util/Format.hpp"

namespace {
    class CTemperatureModule : public IModule {
      public:
        explicit CTemperatureModule(const SModuleConfig& cfg) : IModule(cfg) {}

        void init() override {
            resolveSensor();
            update();
            m_timer.emplace(std::chrono::seconds(std::max<int64_t>(optInt("interval", 10), 1)), [this] { update(); });
        }

        void update() override {
            if (m_path.empty())
                return;

            std::ifstream f(m_path);
            long long     milli = 0;
            if (!f.is_open() || !(f >> milli)) {
                if (m_valid) {
                    m_valid = false;
                    requestRedraw();
                }
                return;
            }

            const long TC = std::lround(milli / 1000.0);
            const long TF = std::lround(milli / 1000.0 * 9.0 / 5.0 + 32.0);

            const auto CRIT = optInt("critical-threshold", 0);
            const auto CLS  = std::string{CRIT > 0 && TC >= CRIT ? "critical" : ""};
            m_tokens        = {{"temperatureC", std::to_string(TC)}, {"temperatureF", std::to_string(TF)}};
            const auto TEXT = Fmt::replaceTokens(opt("format", "{temperatureC}°C"), m_tokens);
            if (m_valid && TEXT == m_text && CLS == m_cls)
                return;
            m_valid = true;
            m_text  = TEXT;
            m_cls   = CLS;
            requestRedraw();
        }

        std::vector<SSegment> segments(PHLMONITOR) override {
            return {SSegment{.text = m_text, .cls = m_cls}};
        }

        std::string tooltip(const SSegment& seg) override {
            if (!optBool("tooltip", true))
                return "";
            if (!seg.tooltip.empty())
                return seg.tooltip;
            return Fmt::replaceTokens(opt("tooltip-format", "{temperatureC}°C / {temperatureF}°F"), m_tokens);
        }

        bool hidden(PHLMONITOR) override {
            return m_path.empty() || !m_valid;
        }

      private:
        void resolveSensor() {
            if (const auto P = opt("hwmon-path"); !P.empty()) {
                m_path = P;
                return;
            }
            if (hasOpt("thermal-zone")) {
                m_path = "/sys/class/thermal/thermal_zone" + std::to_string(optInt("thermal-zone", 0)) + "/temp";
                return;
            }

            // auto-detect: best-ranked known CPU sensor wins (acpitz only as a last resort)
            static constexpr std::array<const char*, 5> PREFERRED = {"coretemp", "k10temp", "zenpower", "cpu_thermal", "acpitz"};

            size_t          bestRank = PREFERRED.size();
            std::error_code ec;
            for (const auto& ENTRY : std::filesystem::directory_iterator("/sys/class/hwmon", ec)) {
                std::ifstream nf(ENTRY.path() / "name");
                std::string   name;
                if (!nf.is_open() || !std::getline(nf, name))
                    continue;
                name = Fmt::trim(name);

                const auto IT = std::find_if(PREFERRED.begin(), PREFERRED.end(), [&name](const char* p) { return name == p; });
                if (IT == PREFERRED.end())
                    continue;

                const auto RANK  = (size_t)std::distance(PREFERRED.begin(), IT);
                const auto INPUT = ENTRY.path() / "temp1_input";
                if (RANK < bestRank && std::filesystem::exists(INPUT, ec)) {
                    bestRank = RANK;
                    m_path   = INPUT.string();
                }
            }
        }

        std::string                        m_path;
        std::string                        m_text;
        std::string                        m_cls;
        std::map<std::string, std::string> m_tokens;
        bool                               m_valid = false;
        std::optional<CModuleTimer>        m_timer;
    };
}

UP<IModule> makeTemperatureModule(const SModuleConfig& cfg) {
    return makeUnique<CTemperatureModule>(cfg);
}
