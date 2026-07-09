#include "Factories.hpp"

#include <linux/input-event-codes.h>
#include <string_view>
#include <systemd/sd-bus.h>

#include "../services/DBus.hpp"
#include "../util/Format.hpp"

// power-profiles-daemon (>= 0.20 ships the org.freedesktop.UPower.PowerProfiles
// name; legacy net.hadess is not needed on this target). System bus, fully
// async: one PropertiesChanged match + coalesced Properties.GetAll fetches.

namespace {

    constexpr const char* PPD_DEST    = "org.freedesktop.UPower.PowerProfiles";
    constexpr const char* PPD_PATH    = "/org/freedesktop/UPower/PowerProfiles";
    constexpr const char* PPD_IFACE   = "org.freedesktop.UPower.PowerProfiles";
    constexpr const char* PROPS_IFACE = "org.freedesktop.DBus.Properties";

    struct SProfileEntry {
        std::string name;   // "power-saver" | "balanced" | "performance"
        std::string driver; // per-entry "Driver" value
    };

    class CPowerProfilesModule : public IModule {
      public:
        explicit CPowerProfilesModule(const SModuleConfig& cfg) : IModule(cfg) {}
        ~CPowerProfilesModule() override;

        void                  init() override;
        void                  update() override;
        std::vector<SSegment> segments(PHLMONITOR mon) override;
        bool                  hidden(PHLMONITOR mon) override;
        void                  onClick(uint32_t button, const SSegment& seg, PHLMONITOR mon) override;

      private:
        void        fetchAll();
        void        onGetAllReply(sd_bus_message* m);
        void        setProfile(const std::string& profile);
        std::string activeDriver() const;

        static int  sOnPropertiesChanged(sd_bus_message* m, void* userdata, sd_bus_error* retError);
        static int  sOnGetAllReply(sd_bus_message* m, void* userdata, sd_bus_error* retError);
        static int  sOnSetReply(sd_bus_message* m, void* userdata, sd_bus_error* retError);

        // slots carry raw `this`: MUST be unref'd (cancelled) before members die
        sd_bus_slot*               m_matchSlot = nullptr;
        sd_bus_slot*               m_callSlot  = nullptr; // in-flight GetAll
        sd_bus_slot*               m_setSlot   = nullptr; // in-flight Properties.Set

        bool                       m_ready = false;
        std::string                m_active;
        std::vector<SProfileEntry> m_profiles; // daemon order; cycling follows it
    };

    CPowerProfilesModule::~CPowerProfilesModule() {
        m_matchSlot = sd_bus_slot_unref(m_matchSlot);
        m_callSlot  = sd_bus_slot_unref(m_callSlot);
        m_setSlot   = sd_bus_slot_unref(m_setSlot);
    }

    void CPowerProfilesModule::init() {
        const auto BUS = DBus::system();
        if (!BUS)
            return; // no system bus: module stays hidden

        sd_bus_match_signal(BUS, &m_matchSlot, PPD_DEST, PPD_PATH, PROPS_IFACE, "PropertiesChanged", sOnPropertiesChanged, this);
        fetchAll();
    }

    void CPowerProfilesModule::update() {
        fetchAll();
    }

    void CPowerProfilesModule::fetchAll() {
        const auto BUS = DBus::system();
        if (!BUS)
            return;

        // coalesce: drop any in-flight GetAll before issuing a new one
        m_callSlot = sd_bus_slot_unref(m_callSlot);
        if (sd_bus_call_method_async(BUS, &m_callSlot, PPD_DEST, PPD_PATH, PROPS_IFACE, "GetAll", sOnGetAllReply, this, "s", PPD_IFACE) < 0)
            return;
        DBus::flush(BUS);
    }

    // reply dispatch happens inside sd_bus_process on the compositor main thread
    void CPowerProfilesModule::onGetAllReply(sd_bus_message* m) {
        if (sd_bus_message_is_method_error(m, nullptr))
            return; // daemon absent / error: keep hidden (or last good state)

        std::string                active = m_active;
        std::vector<SProfileEntry> profiles;
        bool                       gotProfiles = false;

        if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}") <= 0)
            return;

        while (sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv") > 0) {
            const char* key = nullptr;
            if (sd_bus_message_read(m, "s", &key) < 0 || !key)
                return;

            if (std::string_view{key} == "ActiveProfile" && sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "s") > 0) {
                const char* v = nullptr;
                if (sd_bus_message_read(m, "s", &v) >= 0 && v)
                    active = v;
                sd_bus_message_exit_container(m);
            } else if (std::string_view{key} == "Profiles" && sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "aa{sv}") > 0) {
                gotProfiles = true;
                if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "a{sv}") > 0) {
                    while (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}") > 0) {
                        SProfileEntry entry;
                        while (sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv") > 0) {
                            const char* k = nullptr;
                            if (sd_bus_message_read(m, "s", &k) < 0 || !k)
                                return;
                            const bool WANTED = std::string_view{k} == "Profile" || std::string_view{k} == "Driver";
                            if (WANTED && sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "s") > 0) {
                                const char* v = nullptr;
                                if (sd_bus_message_read(m, "s", &v) >= 0 && v)
                                    (std::string_view{k} == "Profile" ? entry.name : entry.driver) = v;
                                sd_bus_message_exit_container(m);
                            } else
                                sd_bus_message_skip(m, "v");
                            sd_bus_message_exit_container(m);
                        }
                        sd_bus_message_exit_container(m);
                        if (!entry.name.empty())
                            profiles.emplace_back(std::move(entry));
                    }
                    sd_bus_message_exit_container(m);
                }
                sd_bus_message_exit_container(m);
            } else
                sd_bus_message_skip(m, "v");

            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);

        m_active = active;
        if (gotProfiles)
            m_profiles = std::move(profiles);
        m_ready = !m_active.empty();
        requestRedraw();
    }

    void CPowerProfilesModule::setProfile(const std::string& profile) {
        const auto BUS = DBus::system();
        if (!BUS)
            return;

        m_setSlot = sd_bus_slot_unref(m_setSlot);
        if (sd_bus_call_method_async(BUS, &m_setSlot, PPD_DEST, PPD_PATH, PROPS_IFACE, "Set", sOnSetReply, this, "ssv", PPD_IFACE, "ActiveProfile", "s",
                                     profile.c_str()) < 0)
            return;
        DBus::flush(BUS);

        // No optimistic local mutation: the daemon's PropertiesChanged -> GetAll
        // round trip is authoritative, and sOnSetReply re-fetches on error. A
        // rejected Set therefore never leaves a phantom profile on the bar.
    }

    std::string CPowerProfilesModule::activeDriver() const {
        for (const auto& P : m_profiles) {
            if (P.name == m_active)
                return P.driver;
        }
        return "";
    }

    bool CPowerProfilesModule::hidden(PHLMONITOR) {
        return !m_ready;
    }

    std::vector<SSegment> CPowerProfilesModule::segments(PHLMONITOR) {
        if (!m_ready)
            return {};

        const auto ICON = opt("format-icons." + m_active, opt("format-icons.default", m_active));

        const std::map<std::string, std::string> TOKENS = {
            {"profile", m_active},
            {"icon", ICON},
            {"driver", activeDriver()},
            {"text", m_active},
        };

        SSegment seg;
        seg.text    = Fmt::replaceTokens(opt("format", "{icon}"), TOKENS);
        seg.cls     = m_active; // color.performance / color.power-saver overrides
        seg.tooltip = Fmt::replaceTokens(opt("tooltip-format", "Power profile: {profile}"), TOKENS);
        return {seg};
    }

    void CPowerProfilesModule::onClick(uint32_t button, const SSegment& seg, PHLMONITOR mon) {
        if (button != BTN_LEFT || hasOpt("on-click")) {
            IModule::onClick(button, seg, mon);
            return;
        }

        // waybar built-in: left click cycles through Profiles in daemon order
        if (!m_ready || m_profiles.empty())
            return;

        size_t idx = 0;
        for (size_t i = 0; i < m_profiles.size(); ++i) {
            if (m_profiles[i].name == m_active) {
                idx = i;
                break;
            }
        }
        setProfile(m_profiles[(idx + 1) % m_profiles.size()].name);
    }

    int CPowerProfilesModule::sOnPropertiesChanged(sd_bus_message*, void* userdata, sd_bus_error*) {
        // coalescing: any property change re-fetches everything
        static_cast<CPowerProfilesModule*>(userdata)->fetchAll();
        return 0;
    }

    int CPowerProfilesModule::sOnGetAllReply(sd_bus_message* m, void* userdata, sd_bus_error*) {
        static_cast<CPowerProfilesModule*>(userdata)->onGetAllReply(m);
        return 1;
    }

    int CPowerProfilesModule::sOnSetReply(sd_bus_message* m, void* userdata, sd_bus_error*) {
        // A successful Set emits PropertiesChanged -> GetAll (authoritative). An
        // error emits nothing, so re-fetch here to resync m_active and avoid the
        // display drifting away from the daemon's real ActiveProfile.
        if (sd_bus_message_is_method_error(m, nullptr))
            static_cast<CPowerProfilesModule*>(userdata)->fetchAll();
        return 1;
    }
}

UP<IModule> makePowerProfilesModule(const SModuleConfig& cfg) {
    return makeUnique<CPowerProfilesModule>(cfg);
}
