#include "Factories.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "../globals.hpp"
#include "../util/Format.hpp"
#include "GpuBackend.hpp"

namespace {
    // CHyprColor channels are 0..1; emit "#RRGGBB" for Pango markup.
    // (twin of the same helper in Clock.cpp -- kept local per module so
    // Format.hpp stays free of compositor types)
    std::string colorHex(const CHyprColor& c) {
        const auto TO255 = [](double v) { return (int)std::lround(std::clamp(v, 0.0, 1.0) * 255.0); };
        char       buf[8];
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", TO255(c.r), TO255(c.g), TO255(c.b));
        return buf;
    }

    std::string gib(unsigned long long bytes) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", (double)bytes / (1024.0 * 1024.0 * 1024.0));
        return buf;
    }

    std::string watts(double v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f", v);
        return buf;
    }

    class CGpuModule : public IModule {
      public:
        explicit CGpuModule(const SModuleConfig& cfg) : IModule(cfg) {}

        void init() override {
            m_backend = GpuBackend::detect(opt("device"));
            update();
            // GPU load is spikier than CPU load, so this polls faster than the
            // cpu module's 10s default.
            m_timer.emplace(std::chrono::seconds(std::max<int64_t>(optInt("interval", 2), 1)), [this] { update(); });
        }

        void update() override {
            if (!m_backend)
                return;

            GpuBackend::SSample s;
            if (!m_backend->sample(s)) {
                if (m_valid) {
                    m_valid = false;
                    requestRedraw();
                }
                return;
            }

            const auto VRAMPCT = s.vramTotal > 0 ? std::lround(100.0 * (double)s.vramUsed / (double)s.vramTotal) : 0L;

            m_tokens = {
                {"usage", std::to_string(s.usage)},
                {"vramUsed", gib(s.vramUsed)},
                {"vramTotal", gib(s.vramTotal)},
                {"vramPercentage", std::to_string(VRAMPCT)},
                {"name", m_backend->gpuName()},
            };
            if (s.memUtil)
                m_tokens["memUsage"] = std::to_string(*s.memUtil);
            if (s.tempC) {
                m_tokens["temperatureC"] = std::to_string(*s.tempC);
                m_tokens["temperatureF"] = std::to_string(std::lround(*s.tempC * 9.0 / 5.0 + 32.0));
            }
            if (s.coreMhz)
                m_tokens["coreClock"] = std::to_string(*s.coreMhz);
            if (s.memMhz)
                m_tokens["memClock"] = std::to_string(*s.memMhz);
            if (s.powerW)
                m_tokens["power"] = watts(*s.powerW);
            if (s.powerLimitW)
                m_tokens["powerLimit"] = watts(*s.powerLimitW);
            if (s.fanPct)
                m_tokens["fanSpeed"] = std::to_string(*s.fanPct);
            if (s.fanRpm)
                m_tokens["fanRpm"] = std::to_string(*s.fanRpm);

            const auto  CRIT = optInt("critical-threshold", 0);
            const auto  WARN = optInt("warning-threshold", 0);
            std::string cls;
            if (CRIT > 0 && s.usage >= CRIT)
                cls = "critical";
            else if (WARN > 0 && s.usage >= WARN)
                cls = "warning";

            const auto TEXT  = Fmt::replaceTokens(opt("format", "{usage}%"), m_tokens);
            const auto PANEL = buildPanel(s, VRAMPCT);

            // The panel carries values (power, temp, clocks) that move while the
            // bar text holds still, so a stale open tooltip needs the redraw too.
            if (m_valid && TEXT == m_text && cls == m_cls && PANEL == m_panel)
                return;
            m_valid = true;
            m_text  = TEXT;
            m_cls   = cls;
            m_panel = PANEL;
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
            if (hasOpt("tooltip-format"))
                return Fmt::replaceTokens(opt("tooltip-format"), m_tokens);
            return m_panel;
        }

        // The default panel is colored Pango markup. A user-supplied
        // tooltip-format is plain text and must NOT be reinterpreted as markup:
        // its {name} token holds driver-provided text that would otherwise be a
        // markup-injection sink.
        bool tooltipIsMarkup() const override {
            return !hasOpt("tooltip-format");
        }

        bool hidden(PHLMONITOR) override {
            return !m_backend || !m_valid;
        }

      private:
        // 16-cell bar: accent for the used share, accent-dim for the remainder.
        static std::string vramBar(double frac, const std::string& accentHex, const std::string& dimHex) {
            constexpr int CELLS  = 16;
            const int     FILLED = std::clamp((int)std::lround(std::clamp(frac, 0.0, 1.0) * CELLS), 0, CELLS);

            std::string out = "<span foreground=\"" + accentHex + "\">";
            for (int i = 0; i < FILLED; ++i)
                out += "█";
            out += "</span><span foreground=\"" + dimHex + "\">";
            for (int i = FILLED; i < CELLS; ++i)
                out += "░";
            out += "</span>";
            return out;
        }

        // Hover panel as Pango markup. Every line past the VRAM bar is emitted
        // only when the backend actually supplied the value, so an AMD APU (no
        // hwmon) degrades to name + VRAM + utilization instead of showing zeroes.
        std::string buildPanel(const GpuBackend::SSample& s, long vramPct) const {
            const std::string ACCENT = colorHex(cfgColor(g_cfg.colAccent));
            const std::string DIM    = colorHex(cfgColor(g_cfg.colAccentDim));

            // driver-provided text: escape before it enters markup
            std::string out = "<b><span foreground=\"" + ACCENT + "\">" + Fmt::escapeMarkup(m_backend->gpuName()) + "</span></b>";

            out += "\nVRAM  " + vramBar(s.vramTotal > 0 ? (double)s.vramUsed / (double)s.vramTotal : 0.0, ACCENT, DIM);
            out += "\n      " + gib(s.vramUsed) + " / " + gib(s.vramTotal) + " GiB  (" + std::to_string(vramPct) + "%)";

            out += "\nUtilization  " + std::to_string(s.usage) + "%";
            if (s.memUtil)
                out += "   Mem ctrl  " + std::to_string(*s.memUtil) + "%";

            if (s.coreMhz) {
                out += "\nCore  " + std::to_string(*s.coreMhz) + " MHz";
                if (s.memMhz)
                    out += "   Mem  " + std::to_string(*s.memMhz) + " MHz";
            }

            if (s.powerW) {
                out += "\nPower  " + watts(*s.powerW);
                if (s.powerLimitW)
                    out += " / " + watts(*s.powerLimitW);
                out += " W";
            }

            if (s.tempC || s.fanPct || s.fanRpm) {
                out += "\n";
                if (s.tempC)
                    out += "Temp  " + std::to_string(*s.tempC) + "°C";
                if (s.fanPct)
                    out += std::string(s.tempC ? "   " : "") + "Fan  " + std::to_string(*s.fanPct) + "%";
                else if (s.fanRpm)
                    out += std::string(s.tempC ? "   " : "") + "Fan  " + std::to_string(*s.fanRpm) + " RPM";
            }
            return out;
        }

        std::unique_ptr<GpuBackend::IBackend> m_backend;
        std::string                           m_text, m_cls, m_panel;
        bool                                  m_valid = false;
        std::map<std::string, std::string>    m_tokens;
        std::optional<CModuleTimer>           m_timer;
    };
}

UP<IModule> makeGpuModule(const SModuleConfig& cfg) {
    return makeUnique<CGpuModule>(cfg);
}
