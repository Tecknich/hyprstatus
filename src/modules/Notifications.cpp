#include "Factories.hpp"

#include <cstdint>
#include <linux/input-event-codes.h>
#include <map>
#include <string>
#include <systemd/sd-bus.h>

#include "../services/DBus.hpp"
#include "../util/Format.hpp"

// SwayNotificationCenter integration (waybar's swaync module parity). Session
// bus, fully event-driven: two Subscribe signal matches keep count+dnd live,
// one async GetSubscribeData primes the initial state. Bell icon + unread
// count; left-click opens swaync's own control center (that panel is swaync's,
// not ours), right-click toggles Do-Not-Disturb, middle clears all.

namespace {

    constexpr const char* SNC_DEST  = "org.erikreider.swaync.cc";
    constexpr const char* SNC_PATH  = "/org/erikreider/swaync/cc";
    constexpr const char* SNC_IFACE = "org.erikreider.swaync.cc";

    class CNotificationsModule : public IModule {
      public:
        explicit CNotificationsModule(const SModuleConfig& cfg) : IModule(cfg) {}
        ~CNotificationsModule() override;

        void                  init() override;
        void                  update() override;
        std::vector<SSegment> segments(PHLMONITOR mon) override;
        bool                  hidden(PHLMONITOR mon) override;
        void                  onClick(uint32_t button, const SSegment& seg, PHLMONITOR mon) override;
        // the bell always acts on click (open control center / toggle DND / clear)
        bool clickable(const SSegment&) const override { return true; }

      private:
        void addMatches(sd_bus* bus); // (re)install both Subscribe matches on the given bus
        void fetch();                 // async GetSubscribeData -> initial state
        void callNoArg(const char* method); // fire-and-forget session-bus call (no reply needed)

        void onSubscribe(sd_bus_message* m);   // Subscribe   (ubb)  = count, dnd, cc_open
        void onSubscribeV2(sd_bus_message* m); // SubscribeV2 (ubbb) = count, dnd, cc_open, inhibited
        void onGetReply(sd_bus_message* m);    // GetSubscribeData -> (bbub) = dnd, cc_open, count, inhibited

        static int sOnSubscribe(sd_bus_message* m, void* userdata, sd_bus_error* retError);
        static int sOnSubscribeV2(sd_bus_message* m, void* userdata, sd_bus_error* retError);
        static int sOnGetReply(sd_bus_message* m, void* userdata, sd_bus_error* retError);

        // slots carry raw `this`: MUST be unref'd (cancelled) before members die
        sd_bus_slot* m_subSlot   = nullptr; // Subscribe match
        sd_bus_slot* m_subV2Slot = nullptr; // SubscribeV2 match
        sd_bus_slot* m_getSlot   = nullptr; // in-flight GetSubscribeData

        uint32_t m_count = 0;
        bool     m_dnd   = false;
        bool     m_ready = false;

        // Liveness guard for the DBus reconnect callback (kept for the plugin's
        // life). This module is destroyed+rebuilt on config reload; the callback
        // captures a weak ref and no-ops once the module (and this guard) are
        // gone, so a stale entry can never touch freed members.
        SP<bool> m_alive = makeShared<bool>(true);
    };

    CNotificationsModule::~CNotificationsModule() {
        m_subSlot   = sd_bus_slot_unref(m_subSlot);
        m_subV2Slot = sd_bus_slot_unref(m_subV2Slot);
        m_getSlot   = sd_bus_slot_unref(m_getSlot);
        // m_alive's SP drops on member destruction -> the DBus reconnect callback no-ops thereafter.
    }

    void CNotificationsModule::addMatches(sd_bus* bus) {
        // Prior slots (if any) referenced the now-dead bus: cancel first. Match
        // BOTH signals: some swaync versions emit only one of them.
        m_subSlot   = sd_bus_slot_unref(m_subSlot);
        m_subV2Slot = sd_bus_slot_unref(m_subV2Slot);
        sd_bus_match_signal(bus, &m_subSlot, SNC_DEST, SNC_PATH, SNC_IFACE, "Subscribe", sOnSubscribe, this);
        sd_bus_match_signal(bus, &m_subV2Slot, SNC_DEST, SNC_PATH, SNC_IFACE, "SubscribeV2", sOnSubscribeV2, this);
    }

    void CNotificationsModule::init() {
        const auto BUS = DBus::session();
        if (!BUS)
            return; // no session bus: module stays hidden

        addMatches(BUS);
        fetch();

        // Self-heal: when the session-bus connection drops and DBus reopens it,
        // re-add the matches on the fresh bus + refetch. Guarded by m_alive so a
        // reload-orphaned copy of this callback is a safe no-op.
        WP<bool> weak = m_alive;
        DBus::onReconnect(false /*session*/, "notifications", [this, weak] {
            if (!weak.lock())
                return; // module was destroyed (e.g. config reload); do not touch it
            const auto BUS = DBus::session();
            if (!BUS)
                return;
            addMatches(BUS);
            fetch();
        });
    }

    void CNotificationsModule::update() {
        fetch();
    }

    void CNotificationsModule::fetch() {
        const auto BUS = DBus::session();
        if (!BUS)
            return;

        // coalesce: drop any in-flight GetSubscribeData before issuing a new one
        m_getSlot = sd_bus_slot_unref(m_getSlot);
        if (sd_bus_call_method_async(BUS, &m_getSlot, SNC_DEST, SNC_PATH, SNC_IFACE, "GetSubscribeData", sOnGetReply, this, "") < 0)
            return;
        DBus::flush(BUS);
    }

    void CNotificationsModule::callNoArg(const char* method) {
        const auto BUS = DBus::session();
        if (!BUS)
            return;
        // Fire-and-forget: no reply slot/callback. The Subscribe signal is the
        // authoritative state update after the action lands.
        sd_bus_call_method_async(BUS, nullptr, SNC_DEST, SNC_PATH, SNC_IFACE, method, nullptr, nullptr, "");
        DBus::flush(BUS);
    }

    // ---- signal / reply dispatch (main thread, inside sd_bus_process) ----

    void CNotificationsModule::onSubscribe(sd_bus_message* m) {
        uint32_t count  = 0;
        int      dnd    = 0;
        int      ccOpen = 0;
        if (sd_bus_message_read(m, "ubb", &count, &dnd, &ccOpen) < 0)
            return;
        m_count = count;
        m_dnd   = dnd != 0;
        m_ready = true;
        requestRedraw();
    }

    void CNotificationsModule::onSubscribeV2(sd_bus_message* m) {
        uint32_t count     = 0;
        int      dnd       = 0;
        int      ccOpen    = 0;
        int      inhibited = 0;
        if (sd_bus_message_read(m, "ubbb", &count, &dnd, &ccOpen, &inhibited) < 0)
            return;
        m_count = count;
        m_dnd   = dnd != 0;
        m_ready = true;
        requestRedraw();
    }

    void CNotificationsModule::onGetReply(sd_bus_message* m) {
        if (sd_bus_message_is_method_error(m, nullptr))
            return; // swaync absent / error: keep hidden (or last good state)

        // GetSubscribeData returns a single STRUCT (bbub) (dnd, cc_open, count,
        // inhibited) — a different order AND shape from the Subscribe signal, so
        // it must be read with the "(...)" struct syntax, not four flat args.
        int      dnd       = 0;
        int      ccOpen    = 0;
        uint32_t count     = 0;
        int      inhibited = 0;
        if (sd_bus_message_read(m, "(bbub)", &dnd, &ccOpen, &count, &inhibited) < 0)
            return;
        m_dnd   = dnd != 0;
        m_count = count;
        m_ready = true;
        requestRedraw();
    }

    bool CNotificationsModule::hidden(PHLMONITOR) {
        return !m_ready;
    }

    std::vector<SSegment> CNotificationsModule::segments(PHLMONITOR) {
        if (!m_ready)
            return {};

        // alt state keys the format-icons map exactly like waybar's swaync module.
        const std::string ALT  = std::string(m_dnd ? "dnd-" : "") + (m_count > 0 ? "notification" : "none");
        const std::string ICON = opt("format-icons." + ALT, opt("format-icons.default", ""));
        const std::string TEXT = m_count > 0 ? std::to_string(m_count) : std::string();

        const std::map<std::string, std::string> TOKENS = {
            {"icon", ICON},
            {"text", TEXT},
            {"count", std::to_string(m_count)},
        };

        SSegment seg;
        seg.text = Fmt::replaceTokens(opt("format", "{icon} {text}"), TOKENS);
        seg.cls  = m_count > 0 ? "notification" : ""; // color.notification override
        // No tooltip by default: only when the user explicitly set one AND
        // tooltips are enabled. (Previously always showed a count/DND string.)
        if (optBool("tooltip", true) && hasOpt("tooltip-format"))
            seg.tooltip = Fmt::replaceTokens(opt("tooltip-format"), TOKENS);
        return {seg};
    }

    void CNotificationsModule::onClick(uint32_t button, const SSegment& seg, PHLMONITOR mon) {
        // If the user configured a command for this button, honor it (defer to
        // the base handler which spawns on-click / on-click-right / -middle).
        const char* USER_KEY = button == BTN_RIGHT ? "on-click-right" : button == BTN_MIDDLE ? "on-click-middle" : "on-click";
        if (hasOpt(USER_KEY)) {
            IModule::onClick(button, seg, mon);
            return;
        }

        const char* METHOD = button == BTN_LEFT     ? "ToggleVisibility"       // open/close swaync's control center
            : button == BTN_RIGHT                   ? "ToggleDnd"               // toggle Do-Not-Disturb
            : button == BTN_MIDDLE                  ? "CloseAllNotifications"   // clear all
                                                    : nullptr;
        if (!METHOD) {
            IModule::onClick(button, seg, mon);
            return;
        }
        callNoArg(METHOD);
    }

    int CNotificationsModule::sOnSubscribe(sd_bus_message* m, void* userdata, sd_bus_error*) {
        static_cast<CNotificationsModule*>(userdata)->onSubscribe(m);
        return 0;
    }

    int CNotificationsModule::sOnSubscribeV2(sd_bus_message* m, void* userdata, sd_bus_error*) {
        static_cast<CNotificationsModule*>(userdata)->onSubscribeV2(m);
        return 0;
    }

    int CNotificationsModule::sOnGetReply(sd_bus_message* m, void* userdata, sd_bus_error*) {
        static_cast<CNotificationsModule*>(userdata)->onGetReply(m);
        return 1;
    }
}

UP<IModule> makeNotificationsModule(const SModuleConfig& cfg) {
    return makeUnique<CNotificationsModule>(cfg);
}
