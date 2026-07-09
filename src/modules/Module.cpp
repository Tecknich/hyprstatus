#include "Module.hpp"

#define WLR_USE_UNSTABLE
#include <hyprland/src/config/supplementary/executor/Executor.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <linux/input-event-codes.h>

#include "../core/BarManager.hpp"
#include "../globals.hpp"

std::string IModule::opt(const std::string& key, const std::string& fallback) const {
    const auto IT = m_config.opts.find(key);
    return IT != m_config.opts.end() ? IT->second : fallback;
}

bool IModule::hasOpt(const std::string& key) const {
    return m_config.opts.contains(key);
}

int64_t IModule::optInt(const std::string& key, int64_t fallback) const {
    const auto V = opt(key);
    if (V.empty())
        return fallback;
    try {
        return std::stoll(V);
    } catch (...) { return fallback; }
}

double IModule::optFloat(const std::string& key, double fallback) const {
    const auto V = opt(key);
    if (V.empty())
        return fallback;
    try {
        return std::stod(V);
    } catch (...) { return fallback; }
}

bool IModule::optBool(const std::string& key, bool fallback) const {
    const auto V = opt(key);
    if (V.empty())
        return fallback;
    return V == "true" || V == "1" || V == "yes" || V == "on";
}

std::string IModule::tooltip(const SSegment& seg) {
    if (!optBool("tooltip", true))
        return "";
    if (!seg.tooltip.empty())
        return seg.tooltip;
    return opt("tooltip-format", "");
}

void IModule::onClick(uint32_t button, const SSegment&, PHLMONITOR) {
    const char* KEY = button == BTN_RIGHT ? "on-click-right" : button == BTN_MIDDLE ? "on-click-middle" : "on-click";
    if (const auto CMD = opt(KEY); !CMD.empty())
        spawn(CMD);
}

void IModule::onScroll(double delta, const SSegment&, PHLMONITOR) {
    if (const auto CMD = opt(delta < 0 ? "on-scroll-up" : "on-scroll-down"); !CMD.empty())
        spawn(CMD);
}

void IModule::requestRedraw() {
    if (g_barManager)
        g_barManager->requestRedraw();
}

void IModule::spawn(const std::string& cmd) {
    if (cmd.empty())
        return;
    Config::Supplementary::executor()->spawn(cmd);
}

// ---- CModuleTimer ----

struct CModuleTimer::SImpl {
    std::chrono::milliseconds interval{1000};
    std::function<void()>     onTick;
    SP<CEventLoopTimer>       timer;
};

CModuleTimer::CModuleTimer(std::chrono::milliseconds interval, std::function<void()> onTick) {
    m_impl           = makeShared<SImpl>();
    m_impl->interval = interval;
    m_impl->onTick   = std::move(onTick);

    WP<SImpl> weak = m_impl;
    m_impl->timer  = makeShared<CEventLoopTimer>(
        interval,
        [weak](SP<CEventLoopTimer> self, void*) {
            const auto IMPL = weak.lock();
            if (!IMPL)
                return;
            IMPL->onTick();
            self->updateTimeout(IMPL->interval);
        },
        nullptr);
    g_pEventLoopManager->addTimer(m_impl->timer);
}

CModuleTimer::~CModuleTimer() {
    if (m_impl && m_impl->timer && g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(m_impl->timer);
}

void CModuleTimer::setInterval(std::chrono::milliseconds interval) {
    if (m_impl)
        m_impl->interval = interval;
}

void CModuleTimer::fireNow() {
    if (!m_impl)
        return;
    m_impl->onTick();
    if (m_impl->timer)
        m_impl->timer->updateTimeout(m_impl->interval);
}
