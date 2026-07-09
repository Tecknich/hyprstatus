#include "Factories.hpp"

#define WLR_USE_UNSTABLE
#include <hyprland/src/Compositor.hpp>

#include <wayland-server-core.h>

#include <linux/netlink.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <string_view>

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

        // MANDATORY: a live fd source after unload dereferences freed memory and
        // crashes the compositor. Remove the wl source BEFORE closing the fd.
        ~CBatteryModule() override {
            if (m_ueventSrc) {
                wl_event_source_remove(m_ueventSrc);
                m_ueventSrc = nullptr;
            }
            if (m_ueventFd >= 0) {
                close(m_ueventFd);
                m_ueventFd = -1;
            }
        }

        void init() override {
            discover();
            const auto INTERVAL = std::max<int64_t>(1, optInt("interval", 30));
            m_timer             = makeUnique<CModuleTimer>(std::chrono::seconds(INTERVAL), [this] { update(); });
            // Kernel uevent listener: power_supply changes (AC plug/unplug) fire an
            // immediate update() instead of waiting for the next poll tick. The poll
            // above stays as a slow fallback for %/time refresh. Best-effort: any
            // failure here just skips the listener; the module still works polled.
            setupUevent();
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
                    const int    N   = (int)glyphs.size();
                    const size_t IDX = (size_t)std::clamp((int)std::floor((double)capacity / (100.0 / N)), 0, N - 1);
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
        // Raw NETLINK_KOBJECT_UEVENT socket (group 1 = kernel broadcast). No
        // libudev dependency. Best-effort: on any failure we skip the listener.
        void setupUevent() {
            const int FD = socket(PF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
            if (FD < 0)
                return;

            struct sockaddr_nl nls{};
            nls.nl_family = AF_NETLINK;
            nls.nl_groups = 1; // udev/kernel broadcast group
            nls.nl_pid    = 0;
            if (bind(FD, reinterpret_cast<struct sockaddr*>(&nls), sizeof(nls)) < 0) {
                close(FD);
                return;
            }

            wl_event_source* src = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, FD, WL_EVENT_READABLE, &onUevent, this);
            if (!src) {
                close(FD);
                return;
            }

            m_ueventFd  = FD;
            m_ueventSrc = src;
        }

        // Drain the socket (NONBLOCK -> read until EAGAIN); if any message payload
        // mentions power_supply, coalesce to a single update(). Never blocks.
        static int onUevent(int fd, uint32_t /*mask*/, void* data) {
            auto* self     = static_cast<CBatteryModule*>(data);
            bool  relevant = false;
            char  buf[8192];
            for (;;) {
                const ssize_t N = recv(fd, buf, sizeof(buf), 0);
                if (N > 0) {
                    // scan raw bytes: uevent payloads are NUL-separated k=v lines
                    if (std::string_view(buf, (size_t)N).find("power_supply") != std::string_view::npos)
                        relevant = true;
                    continue;
                }
                if (N < 0 && errno == EINTR)
                    continue;
                break; // EAGAIN/EWOULDBLOCK = drained, or a hard error
            }
            if (relevant)
                self->update();
            return 0;
        }

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
        int                                m_ueventFd  = -1;
        wl_event_source*                   m_ueventSrc = nullptr;
        std::string                        m_batPath, m_adapterPath;
        bool                               m_hidden = true;
        SSegment                           m_segment;
        std::map<std::string, std::string> m_tokens;
    };
}

UP<IModule> makeBatteryModule(const SModuleConfig& cfg) {
    return makeUnique<CBatteryModule>(cfg);
}
