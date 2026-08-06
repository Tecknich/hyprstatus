#pragma once

// GPU sampling backends: amdgpu via sysfs, NVIDIA via a dlopen'd NVML.
//
// Deliberately free of Hyprland headers (std + POSIX + Fmt only) so it can be
// exercised by a standalone harness without a compositor -- these numbers are
// worth diffing against nvidia-smi / sysfs by hand. Gpu.cpp owns everything
// compositor-facing (config, tokens, markup panel).
//
// All file reads go through the Fmt stdio helpers: no <fstream>/<sstream>, whose
// locale facets make the .so non-dlclose-able (issue #12).

#include <dirent.h>
#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../util/Format.hpp"

namespace GpuBackend {

    inline std::optional<long long> readNumber(const std::string& path) {
        const auto RAW = Fmt::readFile(path);
        if (RAW.empty())
            return std::nullopt;
        return Fmt::toLL(Fmt::trim(RAW));
    }

    inline std::string lower(std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    // "0000:01:00.0" style; used to route a `device` option to the right backend
    inline bool looksLikePciAddr(const std::string& s) {
        return s.find(':') != std::string::npos && s.find('.') != std::string::npos;
    }

    // value of KEY=... in a sysfs uevent blob
    inline std::string ueventValue(const std::string& blob, const std::string& key) {
        const std::string NEEDLE = key + "=";
        for (const auto& LINE : Fmt::split(blob, '\n')) {
            if (LINE.starts_with(NEEDLE))
                return Fmt::trim(LINE.substr(NEEDLE.size()));
        }
        return "";
    }

    // ---- backend model -----------------------------------------------------

    // One poll of a GPU. usage/vram are the only fields every backend provides;
    // the rest are optional so the hover panel renders whatever a given driver
    // actually exposes (an AMD APU publishes no hwmon at all).
    struct SSample {
        long                  usage    = 0;                // 0..100
        unsigned long long    vramUsed = 0, vramTotal = 0; // bytes
        std::optional<long>   memUtil;                     // memory-controller busy %
        std::optional<long>   tempC;
        std::optional<double> powerW, powerLimitW;
        std::optional<long>   fanPct, fanRpm;
        std::optional<long>   coreMhz, memMhz;
    };

    class IBackend {
      public:
        virtual ~IBackend() = default;
        // Fills s from the driver. False = read failure; the caller keeps the
        // previous sample rather than flashing zeroes.
        virtual bool               sample(SSample& s) = 0;
        virtual const std::string& gpuName() const    = 0;
        // Does this device answer to a user-supplied `device` token?
        virtual bool matches(const std::string& lowerQuery) const = 0;
        // Ranking key for auto-selection: larger = preferred (discrete first).
        virtual unsigned long long rank() const = 0;
    };

    // ---- AMD (amdgpu sysfs) ------------------------------------------------

    class CAmdgpu : public IBackend {
      public:
        // Requires gpu_busy_percent: without it there is no usage number to
        // show, so such a card is not a candidate at all.
        static std::unique_ptr<CAmdgpu> tryCreate(const std::string& card) {
            const std::string BASE = "/sys/class/drm/" + card + "/device";
            if (!readNumber(BASE + "/gpu_busy_percent").has_value())
                return nullptr;

            auto b     = std::make_unique<CAmdgpu>();
            b->m_card  = card;
            b->m_base  = BASE;
            b->m_hwmon = findHwmon(BASE);
            b->m_pci   = ueventValue(Fmt::readFile(BASE + "/uevent"), "PCI_SLOT_NAME");
            b->m_vramTotal = (unsigned long long)readNumber(BASE + "/mem_info_vram_total").value_or(0);
            // sysfs carries no marketing name; the slot keeps it unique and
            // still substring-matches "amd".
            b->m_name = b->m_pci.empty() ? "AMD GPU (" + card + ")" : "AMD GPU (" + b->m_pci + ")";
            return b;
        }

        bool sample(SSample& s) override {
            const auto BUSY = readNumber(m_base + "/gpu_busy_percent");
            if (!BUSY)
                return false;
            s.usage = std::clamp((long)*BUSY, 0L, 100L);

            s.vramUsed  = (unsigned long long)readNumber(m_base + "/mem_info_vram_used").value_or(0);
            s.vramTotal = (unsigned long long)readNumber(m_base + "/mem_info_vram_total").value_or(0);

            if (const auto M = readNumber(m_base + "/mem_busy_percent"))
                s.memUtil = std::clamp((long)*M, 0L, 100L);

            s.coreMhz = starredClock(m_base + "/pp_dpm_sclk");
            s.memMhz  = starredClock(m_base + "/pp_dpm_mclk");

            if (!m_hwmon.empty()) {
                if (const auto T = readNumber(m_hwmon + "/temp1_input"))
                    s.tempC = std::lround(*T / 1000.0); // millidegrees
                // power1_average is the sampled draw; power1_input instantaneous
                auto pw = readNumber(m_hwmon + "/power1_average");
                if (!pw)
                    pw = readNumber(m_hwmon + "/power1_input");
                if (pw)
                    s.powerW = (double)*pw / 1e6; // microwatts
                if (const auto C = readNumber(m_hwmon + "/power1_cap"))
                    s.powerLimitW = (double)*C / 1e6;
                if (const auto R = readNumber(m_hwmon + "/fan1_input"))
                    s.fanRpm = (long)*R;
                if (const auto P = readNumber(m_hwmon + "/pwm1"))
                    s.fanPct = std::clamp((long)std::lround(*P * 100.0 / 255.0), 0L, 100L);
                // freq*_input fills in for cards without pp_dpm tables
                if (!s.coreMhz)
                    if (const auto F = readNumber(m_hwmon + "/freq1_input"))
                        s.coreMhz = std::lround(*F / 1e6); // Hz
                if (!s.memMhz)
                    if (const auto F = readNumber(m_hwmon + "/freq2_input"))
                        s.memMhz = std::lround(*F / 1e6);
            }
            return true;
        }

        const std::string& gpuName() const override {
            return m_name;
        }

        bool matches(const std::string& lowerQuery) const override {
            return lowerQuery == lower(m_card) || lowerQuery == lower(m_pci) || lower(m_name).find(lowerQuery) != std::string::npos;
        }

        // VRAM size orders discrete Radeons above an APU's small carve-out.
        unsigned long long rank() const override {
            return m_vramTotal;
        }

      private:
        // first hwmonN under <device>/hwmon; absent on APUs
        static std::string findHwmon(const std::string& base) {
            const std::string HWDIR = base + "/hwmon";
            DIR*              d     = ::opendir(HWDIR.c_str());
            if (!d)
                return "";
            std::string out;
            while (const dirent* E = ::readdir(d)) {
                if (std::string_view(E->d_name).starts_with("hwmon")) {
                    out = HWDIR + "/" + E->d_name;
                    break;
                }
            }
            ::closedir(d);
            return out;
        }

        // pp_dpm_* lists every DPM level, the active one flagged with '*':
        //   0: 400Mhz
        //   1: 600Mhz *
        static std::optional<long> starredClock(const std::string& path) {
            for (const auto& LINE : Fmt::split(Fmt::readFile(path), '\n')) {
                if (LINE.find('*') == std::string::npos)
                    continue;
                const auto COLON = LINE.find(':');
                if (COLON == std::string::npos)
                    continue;
                if (const auto V = Fmt::toLL(Fmt::trim(LINE.substr(COLON + 1))))
                    return (long)*V;
            }
            return std::nullopt;
        }

        std::string        m_card, m_base, m_hwmon, m_pci, m_name;
        unsigned long long m_vramTotal = 0;
    };

    // ---- NVIDIA (NVML, dlopen'd) -------------------------------------------
    //
    // dlopen rather than link: libnvidia-ml stays out of CMakeLists, so the
    // plugin still builds in the Arch CI container and on AMD-only machines,
    // where this backend simply finds no device and the module hides.
    //
    // The handle is deliberately NEVER dlclose'd. NVML holds driver-side state
    // and we register no callbacks with it, so leaving it mapped past
    // PLUGIN_EXIT is inert; unmapping a driver library that other loaded code
    // may share is the riskier half of the plugin-unload hazard (issue #12).

    struct SNvmlUtil {
        unsigned gpu = 0, memory = 0;
    };
    struct SNvmlMem { // nvmlMemory_t v1 layout
        unsigned long long total = 0, free = 0, used = 0;
    };

    // nvmlMemory_v2_t. Preferred over v1 because v1's `used` silently folds in
    // the driver's reserved carve-out (~458 MiB on a 4090), which reads as half
    // a gigabyte of phantom usage next to what nvidia-smi reports. v2 breaks
    // `reserved` out so `used` matches nvidia-smi's memory.used exactly.
    struct SNvmlMemV2 {
        unsigned           version = 0;
        unsigned long long total = 0, reserved = 0, free = 0, used = 0;
    };

    struct SNvml {
        void* lib = nullptr;
        // required
        int (*init)()                    = nullptr;
        int (*count)(unsigned*)          = nullptr;
        int (*byIndex)(unsigned, void**) = nullptr;
        int (*util)(void*, SNvmlUtil*)   = nullptr;
        int (*mem)(void*, SNvmlMem*)     = nullptr;
        // optional
        int (*memV2)(void*, SNvmlMemV2*)    = nullptr;
        int (*byPci)(const char*, void**)   = nullptr;
        int (*name)(void*, char*, unsigned) = nullptr;
        int (*temp)(void*, int, unsigned*)  = nullptr;
        int (*clock)(void*, int, unsigned*) = nullptr;
        int (*power)(void*, unsigned*)      = nullptr;
        int (*powerCap)(void*, unsigned*)   = nullptr;
        int (*fan)(void*, unsigned*)        = nullptr;
    };

    // Loaded once, on the main thread, at first module init.
    inline const SNvml* nvml() {
        static SNvml N = [] {
            SNvml n;
            n.lib = ::dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
            if (!n.lib)
                return n;

            const auto SYM = [&n](const char* s) { return ::dlsym(n.lib, s); };
            n.init         = reinterpret_cast<decltype(n.init)>(SYM("nvmlInit_v2"));
            n.count        = reinterpret_cast<decltype(n.count)>(SYM("nvmlDeviceGetCount_v2"));
            n.byIndex      = reinterpret_cast<decltype(n.byIndex)>(SYM("nvmlDeviceGetHandleByIndex_v2"));
            n.util         = reinterpret_cast<decltype(n.util)>(SYM("nvmlDeviceGetUtilizationRates"));
            n.mem          = reinterpret_cast<decltype(n.mem)>(SYM("nvmlDeviceGetMemoryInfo"));
            n.memV2        = reinterpret_cast<decltype(n.memV2)>(SYM("nvmlDeviceGetMemoryInfo_v2"));
            n.byPci        = reinterpret_cast<decltype(n.byPci)>(SYM("nvmlDeviceGetHandleByPciBusId_v2"));
            n.name         = reinterpret_cast<decltype(n.name)>(SYM("nvmlDeviceGetName"));
            n.temp         = reinterpret_cast<decltype(n.temp)>(SYM("nvmlDeviceGetTemperature"));
            n.clock        = reinterpret_cast<decltype(n.clock)>(SYM("nvmlDeviceGetClockInfo"));
            n.power        = reinterpret_cast<decltype(n.power)>(SYM("nvmlDeviceGetPowerUsage"));
            n.powerCap     = reinterpret_cast<decltype(n.powerCap)>(SYM("nvmlDeviceGetEnforcedPowerLimit"));
            n.fan          = reinterpret_cast<decltype(n.fan)>(SYM("nvmlDeviceGetFanSpeed"));

            if (!n.init || !n.count || !n.byIndex || !n.util || !n.mem || n.init() != 0)
                n = SNvml{}; // unusable; the library stays mapped by design
            return n;
        }();
        return N.init ? &N : nullptr;
    }

    inline std::string nvmlDeviceName(void* dev) {
        const auto* N = nvml();
        char        buf[96]{}; // NVML_DEVICE_NAME_V2_BUFFER_SIZE
        if (N && N->name && N->name(dev, buf, sizeof(buf)) == 0 && buf[0])
            return buf;
        return "NVIDIA GPU";
    }

    class CNvml : public IBackend {
      public:
        CNvml(void* dev, std::string name) : m_dev(dev), m_name(std::move(name)) {}

        bool sample(SSample& s) override {
            const auto* N = nvml();
            if (!N || !m_dev)
                return false;

            SNvmlUtil u;
            if (N->util(m_dev, &u) != 0)
                return false;
            s.usage   = std::clamp((long)u.gpu, 0L, 100L);
            s.memUtil = std::clamp((long)u.memory, 0L, 100L);

            // v2 first (see SNvmlMemV2); a driver too old for it, or a version
            // word this build guessed wrong, fails cleanly and falls back to v1.
            bool gotMem = false;
            if (N->memV2) {
                SNvmlMemV2 m2;
                m2.version = (unsigned)(sizeof(SNvmlMemV2) | (2u << 24)); // NVML_STRUCT_VERSION(Memory, 2)
                if (N->memV2(m_dev, &m2) == 0 && m2.total > 0) {
                    s.vramUsed  = m2.used;
                    s.vramTotal = m2.total;
                    gotMem      = true;
                }
            }
            if (!gotMem) {
                SNvmlMem m;
                if (N->mem(m_dev, &m) == 0) {
                    s.vramUsed  = m.used;
                    s.vramTotal = m.total;
                }
            }

            unsigned v = 0;
            if (N->temp && N->temp(m_dev, 0 /*NVML_TEMPERATURE_GPU*/, &v) == 0)
                s.tempC = (long)v;
            if (N->clock && N->clock(m_dev, 0 /*NVML_CLOCK_GRAPHICS*/, &v) == 0)
                s.coreMhz = (long)v;
            if (N->clock && N->clock(m_dev, 2 /*NVML_CLOCK_MEM*/, &v) == 0)
                s.memMhz = (long)v;
            if (N->power && N->power(m_dev, &v) == 0)
                s.powerW = (double)v / 1000.0; // milliwatts
            if (N->powerCap && N->powerCap(m_dev, &v) == 0)
                s.powerLimitW = (double)v / 1000.0;
            if (N->fan && N->fan(m_dev, &v) == 0)
                s.fanPct = std::clamp((long)v, 0L, 100L);
            return true;
        }

        const std::string& gpuName() const override {
            return m_name;
        }

        bool matches(const std::string& lowerQuery) const override {
            return lower(m_name).find(lowerQuery) != std::string::npos;
        }

        // Any NVML device outranks any amdgpu card: NVIDIA parts are discrete in
        // every configuration this module can see, and VRAM byte counts are not
        // comparable across the two backends.
        unsigned long long rank() const override {
            return ~0ULL;
        }

      private:
        void*       m_dev = nullptr;
        std::string m_name;
    };

    // ---- discovery ---------------------------------------------------------

    inline std::vector<std::unique_ptr<IBackend>> enumerateNvml() {
        std::vector<std::unique_ptr<IBackend>> out;
        const auto*                            N = nvml();
        if (!N)
            return out;

        unsigned c = 0;
        if (N->count(&c) != 0)
            return out;
        for (unsigned i = 0; i < c; ++i) {
            void* dev = nullptr;
            if (N->byIndex(i, &dev) != 0 || !dev)
                continue;
            out.emplace_back(std::make_unique<CNvml>(dev, nvmlDeviceName(dev)));
        }
        return out;
    }

    inline std::vector<std::unique_ptr<IBackend>> enumerateAmdgpu() {
        std::vector<std::unique_ptr<IBackend>> out;

        DIR* d = ::opendir("/sys/class/drm");
        if (!d)
            return out;
        std::vector<std::string> cards;
        while (const dirent* E = ::readdir(d)) {
            const std::string_view NAME = E->d_name;
            // cardN only; skip the cardN-<CONNECTOR> child dirs
            if (!NAME.starts_with("card") || NAME.find('-') != std::string_view::npos)
                continue;
            if (NAME.size() > 4 && std::all_of(NAME.begin() + 4, NAME.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; }))
                cards.emplace_back(NAME);
        }
        ::closedir(d);

        std::ranges::sort(cards); // stable, card0 before card1
        for (const auto& C : cards) {
            if (auto b = CAmdgpu::tryCreate(C))
                out.emplace_back(std::move(b));
        }
        return out;
    }

    // `device` accepts a vendor keyword (nvidia/amd), a PCI slot
    // ("0000:01:00.0"), a DRM card ("card1"), or a substring of the GPU name.
    // An explicit request that matches nothing yields no backend -- the module
    // hides rather than silently reporting a different GPU.
    inline std::unique_ptr<IBackend> detect(const std::string& deviceOpt) {
        const std::string RAW = Fmt::trim(deviceOpt);
        const std::string DEV = lower(RAW);

        auto nv  = enumerateNvml();
        auto amd = enumerateAmdgpu();

        if (!DEV.empty()) {
            if (DEV == "nvidia" || DEV == "nvml")
                return nv.empty() ? nullptr : std::move(nv.front());
            if (DEV == "amd" || DEV == "amdgpu" || DEV == "radeon")
                return amd.empty() ? nullptr : std::move(amd.front());

            // NVML exposes a by-bus-id lookup, which spares us nvmlPciInfo_t's
            // version-sensitive layout.
            if (looksLikePciAddr(DEV)) {
                if (const auto* N = nvml(); N && N->byPci) {
                    void* dev = nullptr;
                    if (N->byPci(RAW.c_str(), &dev) == 0 && dev)
                        return std::make_unique<CNvml>(dev, nvmlDeviceName(dev));
                }
            }

            for (auto* list : {&nv, &amd})
                for (auto& b : *list)
                    if (b && b->matches(DEV))
                        return std::move(b);
            return nullptr;
        }

        // auto: highest rank wins (NVML discrete, then Radeons by VRAM, then APUs)
        std::unique_ptr<IBackend> best;
        for (auto* list : {&nv, &amd})
            for (auto& b : *list)
                if (b && (!best || b->rank() > best->rank()))
                    best = std::move(b);
        return best;
    }

} // namespace GpuBackend
