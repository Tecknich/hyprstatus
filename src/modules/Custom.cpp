#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Factories.hpp"

#include "../services/Exec.hpp"
#include "../services/Signals.hpp"
#include "../util/Format.hpp"
#include "../util/Json.hpp"

// Waybar `custom/*` engine: static (format-only), polled (interval N | "once")
// and streaming (exec, no interval) children, exec-if gating, return-type
// json, format-icons keyed by the JSON `alt` field, RT-signal refresh.
class CCustomModule : public IModule {
  public:
    explicit CCustomModule(const SModuleConfig& cfg) : IModule(cfg) {}

    ~CCustomModule() override {
        // timers first (no further cycles), then processes: destroying a
        // CAsyncProcess removes its fd source and SIGKILLs streaming children.
        m_timer.reset();
        m_restartTimer.reset();
        m_gateProc.reset();
        m_execProc.reset();
    }

    void init() override {
        m_exec          = opt("exec");
        m_execIf        = opt("exec-if");
        m_format        = opt("format", "{}");
        m_json          = opt("return-type") == "json";
        m_hideWhenEmpty = optBool("hide-when-empty", !m_exec.empty());
        m_maxLength     = (size_t)std::max<int64_t>(0, optInt("max-length", 0));
        m_hidden        = !m_exec.empty() && m_hideWhenEmpty; // nothing to show until first output

        if (hasOpt("signal")) {
            // Capturing `this` is safe ONLY because BarManager::rebuild calls
            // RtSignals::unsubscribeAll() BEFORE destroying modules, so the
            // subscription never outlives the module. Do not reorder that.
            if (const auto N = optInt("signal", -1); N >= 0)
                RtSignals::subscribe((int)N, [this]() { update(); });
        }

        if (m_exec.empty()) {
            m_mode = MODE_STATIC; // identity module: format only, always visible
            return;
        }

        if (!hasOpt("interval")) {
            m_mode = MODE_STREAM;
            ensureStream();
        } else if (opt("interval") == "once") {
            m_mode = MODE_ONCE;
            cycle();
        } else {
            m_mode        = MODE_POLL;
            const auto MS = std::max<int64_t>(100, (int64_t)(optFloat("interval", 1) * 1000.0));
            m_timer       = std::make_unique<CModuleTimer>(std::chrono::milliseconds(MS), [this]() { cycle(); });
            cycle();
        }
    }

    void update() override {
        switch (m_mode) {
            case MODE_STATIC: break;
            case MODE_ONCE: cycle(); break;
            case MODE_POLL:
                if (m_timer)
                    m_timer->fireNow(); // run a cycle now + re-align the next poll
                else
                    cycle();
                break;
            case MODE_STREAM: ensureStream(); break; // restart child if dead, else no-op
        }
    }

    bool hidden(PHLMONITOR) override {
        return m_hidden;
    }

    std::vector<SSegment> segments(PHLMONITOR) override {
        SSegment seg;
        seg.text    = Fmt::replaceTokens(m_format, tokens());
        seg.cls     = m_class;
        seg.tooltip = m_tooltipText;
        return {seg};
    }

    std::string tooltip(const SSegment& seg) override {
        if (!optBool("tooltip", true))
            return "";
        // waybar order: tooltip-format (tokenized) wins over the JSON tooltip
        if (const auto TF = opt("tooltip-format"); !TF.empty())
            return Fmt::replaceTokens(TF, tokens());
        return seg.tooltip;
    }

  private:
    enum eMode : uint8_t {
        MODE_STATIC = 0, // no exec
        MODE_ONCE,       // interval = once
        MODE_POLL,       // interval = N seconds
        MODE_STREAM,     // exec set, no interval
    };

    std::map<std::string, std::string> tokens() const {
        const auto ICON = opt("format-icons." + m_alt, opt("format-icons.default", ""));
        return {
            {"", m_text}, {"text", m_text}, {"alt", m_alt}, {"percentage", m_percentage}, {"icon", ICON}, {"tooltip", m_tooltipText},
        };
    }

    bool busy() const {
        return (m_gateProc && m_gateProc->running()) || (m_execProc && m_execProc->running());
    }

    // one poll/once cycle: [exec-if gate] -> exec -> parse -> apply
    void cycle() {
        if (busy()) // previous child still in flight: skip this tick
            return;
        if (!m_execIf.empty())
            runGate([this]() { runExecOnce(); });
        else
            runExecOnce();
    }

    // exit codes are unreliable under SA_NOCLDWAIT — the sentinel IS the
    // mechanism. Gate failure hides the module and skips exec entirely
    // (custom/gpu relies on this to not wake the runtime-suspended dGPU).
    void runGate(std::function<void()> onOk) {
        const auto CMD = "{ " + m_execIf + " ; } >/dev/null 2>&1 && echo __HS_OK__ || echo __HS_NO__";
        m_gateProc     = CAsyncProcess::run(CMD, [this, onOk = std::move(onOk)](std::string out) {
            if (out.find("__HS_OK__") != std::string::npos)
                onOk();
            else
                setHidden(true);
        });
    }

    void runExecOnce() {
        m_execProc = CAsyncProcess::run(m_exec, [this](std::string out) { applyOutput(std::move(out)); });
    }

    // streaming: (re)spawn the persistent child unless one is alive
    void ensureStream() {
        if (busy())
            return;
        if (!m_execIf.empty())
            runGate([this]() { startStream(); });
        else
            startStream();
    }

    void startStream() {
        m_execProc = CAsyncProcess::stream(
            m_exec, [this](std::string line) { applyOutput(std::move(line)); }, [this](std::string) { onStreamExit(); });
    }

    void onStreamExit() {
        // keep the last output; restart-interval set -> re-stream after N s
        if (!hasOpt("restart-interval"))
            return;
        const auto SECS = std::max<int64_t>(0, optInt("restart-interval", 1));
        m_restartTimer  = std::make_unique<CModuleTimer>(std::chrono::seconds(SECS), [this]() {
            ensureStream();
            m_restartTimer.reset(); // one-shot; the event loop's own SP keeps the firing timer alive through this call
        });
    }

    void applyOutput(std::string raw) {
        std::string text, alt, tip, cls, pct;
        if (m_json) {
            // malformed JSON degrades to an all-empty update
            if (const auto OBJ = MiniJson::parseObject(raw); OBJ) {
                const auto GET = [&](const char* k) {
                    const auto IT = OBJ->find(k);
                    return IT != OBJ->end() ? IT->second : std::string{};
                };
                text = GET("text");
                alt  = GET("alt");
                tip  = GET("tooltip");
                cls  = GET("class");
                pct  = GET("percentage");
            }
        } else
            text = Fmt::trim(raw);

        text = Fmt::truncate(text, m_maxLength);

        const bool HIDDEN = text.empty() && m_hideWhenEmpty;
        if (text == m_text && alt == m_alt && tip == m_tooltipText && cls == m_class && pct == m_percentage && HIDDEN == m_hidden)
            return;

        m_text        = std::move(text);
        m_alt         = std::move(alt);
        m_tooltipText = std::move(tip);
        m_class       = std::move(cls);
        m_percentage  = std::move(pct);
        m_hidden      = HIDDEN;
        requestRedraw();
    }

    void setHidden(bool hidden) {
        if (m_hidden == hidden)
            return;
        m_hidden = hidden;
        requestRedraw();
    }

    // config (resolved once in init)
    eMode       m_mode = MODE_STATIC;
    std::string m_exec, m_execIf, m_format;
    bool        m_json          = false;
    bool        m_hideWhenEmpty = false;
    size_t      m_maxLength     = 0;

    // current state
    std::string m_text, m_alt, m_tooltipText, m_class, m_percentage;
    bool        m_hidden = false;

    std::unique_ptr<CAsyncProcess> m_gateProc, m_execProc;
    std::unique_ptr<CModuleTimer>  m_timer;        // poll ticks
    std::unique_ptr<CModuleTimer>  m_restartTimer; // one-shot stream restart
};

UP<IModule> makeCustomModule(const SModuleConfig& cfg) {
    return makeUnique<CCustomModule>(cfg);
}
