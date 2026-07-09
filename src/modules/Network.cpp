#include "Factories.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <linux/wireless.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>

#include "../util/Format.hpp"

namespace {

    std::optional<long long> readLL(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open())
            return std::nullopt;
        long long v = 0;
        if (!(f >> v))
            return std::nullopt;
        return v;
    }

    std::string defaultRouteIface() {
        std::ifstream f("/proc/net/route");
        if (!f.is_open())
            return "";
        std::string line;
        std::getline(f, line); // header
        while (std::getline(f, line)) {
            std::istringstream iss(line);
            std::string        iface, dest;
            if (!(iss >> iface >> dest))
                continue;
            if (dest == "00000000")
                return iface;
        }
        return "";
    }

    std::string wifiEssid(const std::string& iface) {
        const int FD = socket(AF_INET, SOCK_DGRAM, 0);
        if (FD < 0)
            return "";
        iwreq wrq{};
        char  essid[IW_ESSID_MAX_SIZE + 1] = {0};
        // linux/if.h's ifr_name macro can be compiled out by libc-compat; use the member
        std::strncpy(wrq.ifr_ifrn.ifrn_name, iface.c_str(), IFNAMSIZ - 1);
        wrq.u.essid.pointer = essid;
        wrq.u.essid.length  = IW_ESSID_MAX_SIZE + 1;
        const int RET       = ioctl(FD, SIOCGIWESSID, &wrq);
        close(FD);
        return RET < 0 ? "" : essid;
    }

    struct SWifiSignal {
        std::string strength, dbm;
    };

    SWifiSignal wifiSignal(const std::string& iface) {
        std::ifstream f("/proc/net/wireless");
        if (!f.is_open())
            return {};
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream iss(line);
            std::string        name, status, quality, level;
            if (!(iss >> name >> status >> quality >> level))
                continue;
            if (name != iface + ":")
                continue;
            try {
                // quality is X/70 on cfg80211; level is dBm
                const auto Q = std::stod(quality);
                const auto L = std::stod(level);
                return {std::to_string(std::clamp<long>(std::lround(100.0 * Q / 70.0), 0, 100)), std::to_string(std::lround(L))};
            } catch (...) { return {}; }
        }
        return {};
    }

    struct SIPv4 {
        std::string addr;
        int         cidr = 0;
    };

    SIPv4 ifaceIPv4(const std::string& iface) {
        ifaddrs* addrs = nullptr;
        if (getifaddrs(&addrs) != 0)
            return {};
        SIPv4 out;
        for (auto* a = addrs; a; a = a->ifa_next) {
            if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || iface != a->ifa_name)
                continue;
            char buf[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr, buf, sizeof buf);
            out.addr = buf;
            if (a->ifa_netmask)
                out.cidr = std::popcount((uint32_t)reinterpret_cast<sockaddr_in*>(a->ifa_netmask)->sin_addr.s_addr);
            break;
        }
        freeifaddrs(addrs);
        return out;
    }

    class CNetworkModule : public IModule {
      public:
        explicit CNetworkModule(const SModuleConfig& cfg) : IModule(cfg) {}

        enum class eState : uint8_t {
            WIFI,
            ETHERNET,
            DISCONNECTED,
        };

        void init() override {
            const auto INTERVAL = std::max<int64_t>(1, optInt("interval", 2));
            m_timer             = makeUnique<CModuleTimer>(std::chrono::seconds(INTERVAL), [this] { update(); });
            update();
        }

        void update() override {
            auto iface = opt("interface");
            if (iface.empty())
                iface = defaultRouteIface();

            std::error_code ec;
            const bool      CONNECTED = !iface.empty() && std::filesystem::exists("/sys/class/net/" + iface, ec);

            auto state = eState::DISCONNECTED;
            if (CONNECTED)
                state = std::filesystem::exists("/sys/class/net/" + iface + "/wireless", ec) ? eState::WIFI : eState::ETHERNET;

            // bandwidth: byte-counter deltas over elapsed steady time
            const auto NOW    = std::chrono::steady_clock::now();
            double     rxRate = 0, txRate = 0; // bytes/sec
            if (CONNECTED) {
                const auto RX      = readLL("/sys/class/net/" + iface + "/statistics/rx_bytes").value_or(0);
                const auto TX      = readLL("/sys/class/net/" + iface + "/statistics/tx_bytes").value_or(0);
                const auto ELAPSED = std::chrono::duration<double>(NOW - m_lastSample).count();
                if (m_hasSample && iface == m_lastIface && ELAPSED > 0 && RX >= m_lastRx && TX >= m_lastTx) {
                    rxRate = (double)(RX - m_lastRx) / ELAPSED;
                    txRate = (double)(TX - m_lastTx) / ELAPSED;
                }
                m_lastRx    = RX;
                m_lastTx    = TX;
                m_hasSample = true;
            } else
                m_hasSample = false;
            m_lastIface  = iface;
            m_lastSample = NOW;

            std::string essid, strength, dbm;
            if (state == eState::WIFI) {
                essid          = wifiEssid(iface);
                const auto SIG = wifiSignal(iface);
                strength       = SIG.strength;
                dbm            = SIG.dbm;
            }

            SIPv4 ip;
            if (CONNECTED)
                ip = ifaceIPv4(iface);

            m_tokens = {
                {"ifname", CONNECTED ? iface : ""},
                {"essid", essid},
                {"signalStrength", strength},
                {"signaldBm", dbm},
                {"ipaddr", ip.addr},
                {"cidr", std::to_string(ip.cidr)},
                {"bandwidthDownBits", Fmt::humanBits(rxRate * 8.0)},
                {"bandwidthUpBits", Fmt::humanBits(txRate * 8.0)},
                {"bandwidthDownBytes", Fmt::humanBytes(rxRate)},
                {"bandwidthUpBytes", Fmt::humanBytes(txRate)},
            };
            m_state = state;

            std::string fmt, cls;
            switch (state) {
                case eState::WIFI: fmt = opt("format-wifi", " {essid}"); break;
                case eState::ETHERNET: fmt = opt("format-ethernet", "{ifname}"); break;
                case eState::DISCONNECTED:
                    fmt = opt("format-disconnected", "disconnected");
                    cls = "disconnected";
                    break;
            }

            SSegment seg;
            seg.text = Fmt::replaceTokens(fmt, m_tokens);
            seg.cls  = cls;

            const bool CHANGED = seg.text != m_segment.text || seg.cls != m_segment.cls;
            m_segment          = seg;
            if (CHANGED)
                requestRedraw();
        }

        std::vector<SSegment> segments(PHLMONITOR) override {
            return {m_segment};
        }

        std::string tooltip(const SSegment&) override {
            if (!optBool("tooltip", true))
                return "";
            const char* KEY = m_state == eState::WIFI ? "tooltip-format-wifi" : m_state == eState::ETHERNET ? "tooltip-format-ethernet" : "tooltip-format-disconnected";
            auto        fmt = opt(KEY);
            if (fmt.empty())
                fmt = opt("tooltip-format");
            if (fmt.empty())
                return "";
            return Fmt::replaceTokens(fmt, m_tokens);
        }

      private:
        UP<CModuleTimer>                      m_timer;
        SSegment                              m_segment;
        std::map<std::string, std::string>    m_tokens;
        eState                                m_state = eState::DISCONNECTED;

        std::string                           m_lastIface;
        long long                             m_lastRx = 0, m_lastTx = 0;
        bool                                  m_hasSample = false;
        std::chrono::steady_clock::time_point m_lastSample;
    };
}

UP<IModule> makeNetworkModule(const SModuleConfig& cfg) {
    return makeUnique<CNetworkModule>(cfg);
}
