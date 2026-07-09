#include "Factories.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>

#include "../util/Format.hpp"

namespace {

    constexpr const char* POWER_SUPPLY_DIR = "/sys/class/power_supply";

    std::optional<std::string> readTrimmed(const std::filesystem::path& path) {
        std::ifstream f(path);
        if (!f.is_open())
            return std::nullopt;
        std::string line;
        std::getline(f, line);
        return Fmt::trim(line);
    }

    std::optional<long long> readLL(const std::filesystem::path& path) {
        const auto S = readTrimmed(path);
        if (!S || S->empty())
            return std::nullopt;
        try {
            return std::stoll(*S);
        } catch (...) { return std::nullopt; }
    }

    std::string findSupplyByType(const std::string& type) {
        std::error_code ec;
        for (const auto& ENTRY : std::filesystem::directory_iterator(POWER_SUPPLY_DIR, ec)) {
            if (readTrimmed(ENTRY.path() / "type").value_or("") == type)
                return ENTRY.path().string();
        }
        return "";
    }

    class CBatteryModule : public IModule {
      public:
        explicit CBatteryModule(const SModuleConfig& cfg) : IModule(cfg) {}

        void init() override {
            discover();
            const auto INTERVAL = std::max<int64_t>(1, optInt("interval", 30));
            m_timer             = makeUnique<CModuleTimer>(std::chrono::seconds(INTERVAL), [this] { update(); });
            update();
        }

        void update() override {
            if (m_batPath.empty())
                discover();

            const bool WASHIDDEN = m_hidden;
            if (m_batPath.empty()) {
                m_hidden = true;
                if (!WASHIDDEN)
                    requestRedraw();
                return;
            }

            const auto STATUS = readTrimmed(m_batPath + "/status").value_or("Unknown");

            // both sysfs spellings: energy_* (uWh, power_now uW) and
            // charge_* (uAh, current_now uA) + voltage_now (uV)
            long long now = 0, full = 0, fullDesign = 0;
            double    rate   = 0; // same unit-per-hour as now/full -> hours = amount / rate
            double    powerW = 0;
            if (const auto ENOW = readLL(m_batPath + "/energy_now"); ENOW) {
                now             = *ENOW;
                full            = readLL(m_batPath + "/energy_full").value_or(0);
                fullDesign      = readLL(m_batPath + "/energy_full_design").value_or(0);
                const auto PNOW = std::abs(readLL(m_batPath + "/power_now").value_or(0));
                rate            = PNOW;
                powerW          = PNOW / 1e6;
            } else {
                now             = readLL(m_batPath + "/charge_now").value_or(0);
                full            = readLL(m_batPath + "/charge_full").value_or(0);
                fullDesign      = readLL(m_batPath + "/charge_full_design").value_or(0);
                const auto INOW = std::abs(readLL(m_batPath + "/current_now").value_or(0));
                const auto VNOW = readLL(m_batPath + "/voltage_now").value_or(0);
                rate            = INOW;
                powerW          = (double)INOW * (double)VNOW / 1e12;
            }

            long long capacity = readLL(m_batPath + "/capacity").value_or(full > 0 ? std::llround(100.0 * now / full) : 0);
            capacity           = std::clamp<long long>(capacity, 0, 100);

            const bool ACONLINE = !m_adapterPath.empty() && readLL(m_adapterPath + "/online").value_or(0) == 1;

            std::string timeTo;
            if (rate > 0 && STATUS != "Full") {
                double hours = -1;
                if (STATUS == "Discharging")
                    hours = now / rate;
                else if (STATUS == "Charging" && full > now)
                    hours = (full - now) / rate;
                if (hours >= 0) {
                    long long h = (long long)hours;
                    long long m = std::llround((hours - (double)h) * 60.0);
                    if (m == 60) {
                        h++;
                        m = 0;
                    }
                    timeTo = std::format("{} h {} min {}", h, m, STATUS == "Charging" ? "until full" : "remaining");
                }
            }

            std::string icon;
            if (const auto ICONS = opt("format-icons"); !ICONS.empty()) {
                auto glyphs = Fmt::split(ICONS, ' ');
                std::erase_if(glyphs, [](const std::string& s) { return s.empty(); });
                if (!glyphs.empty()) {
                    const size_t IDX = std::min((size_t)(capacity * (long long)glyphs.size() / 101), glyphs.size() - 1);
                    icon             = glyphs[IDX];
                }
            }

            m_tokens = {
                {"capacity", std::to_string(capacity)},
                {"icon", icon},
                {"power", std::format("{:.1f}", powerW)},
                {"health", fullDesign > 0 ? std::to_string(std::llround(100.0 * full / fullDesign)) : ""},
                {"timeTo", timeTo},
                {"status", STATUS},
            };

            const auto  DEFAULTFMT = opt("format", "{capacity}%");
            std::string fmt        = DEFAULTFMT;
            std::string cls;
            if (STATUS == "Charging") {
                fmt = opt("format-charging", DEFAULTFMT);
                cls = "charging";
            } else if (ACONLINE && (STATUS == "Full" || STATUS == "Not charging")) {
                fmt = opt("format-plugged", DEFAULTFMT);
                cls = "plugged";
            } else if (capacity <= optInt("states.critical", 10))
                cls = "critical";
            else if (capacity <= optInt("states.warning", 20))
                cls = "warning";

            SSegment seg;
            seg.text = Fmt::replaceTokens(fmt, m_tokens);
            seg.cls  = cls;

            m_hidden           = false;
            const bool CHANGED = WASHIDDEN || seg.text != m_segment.text || seg.cls != m_segment.cls;
            m_segment          = seg;
            if (CHANGED)
                requestRedraw();
        }

        std::vector<SSegment> segments(PHLMONITOR) override {
            if (m_hidden)
                return {};
            return {m_segment};
        }

        bool hidden(PHLMONITOR) override {
            return m_hidden;
        }

        std::string tooltip(const SSegment&) override {
            if (m_hidden || !optBool("tooltip", true))
                return "";
            return Fmt::replaceTokens(opt("tooltip-format", "{timeTo}"), m_tokens);
        }

      private:
        void discover() {
            if (const auto BAT = opt("bat"); !BAT.empty()) {
                const auto      PATH = std::string(POWER_SUPPLY_DIR) + "/" + BAT;
                std::error_code ec;
                m_batPath = std::filesystem::exists(PATH, ec) ? PATH : "";
            } else
                m_batPath = findSupplyByType("Battery");

            if (const auto ADAPTER = opt("adapter"); !ADAPTER.empty())
                m_adapterPath = std::string(POWER_SUPPLY_DIR) + "/" + ADAPTER;
            else
                m_adapterPath = findSupplyByType("Mains");
        }

        UP<CModuleTimer>                   m_timer;
        std::string                        m_batPath, m_adapterPath;
        bool                               m_hidden = true;
        SSegment                           m_segment;
        std::map<std::string, std::string> m_tokens;
    };
}

UP<IModule> makeBatteryModule(const SModuleConfig& cfg) {
    return makeUnique<CBatteryModule>(cfg);
}
