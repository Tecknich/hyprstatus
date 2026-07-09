#include "Factories.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <linux/input-event-codes.h>
#include <pulse/pulseaudio.h>

#include <hyprutils/string/String.hpp>

#include "../core/BarManager.hpp"
#include "../globals.hpp"
#include "../services/MainThread.hpp"

// All libpulse callbacks run on the pa_threaded_mainloop's own thread. They may
// only touch SPulseState under its mutex and marshal via MainThread::post with
// a weak_ptr — never compositor state, never the module object. Every pa_* call
// made from the main thread must hold the mainloop lock.
namespace {

    struct SPulseState : std::enable_shared_from_this<SPulseState> {
        std::mutex mtx;

        // display fields (guarded by mtx)
        int         volumePct = 0;
        bool        muted     = false;
        bool        bluetooth = false;
        std::string portKind  = "default"; // speaker | headphone | hdmi | default
        std::string desc;
        std::string sinkName;
        pa_cvolume  cv{};
        bool        ready    = false;
        bool        teardown = false; // guarded by mtx; set before pa teardown

        // pa handles: written on the main thread before the mainloop starts,
        // torn down under the mainloop lock; PA callbacks use their own args.
        pa_threaded_mainloop* mainloop = nullptr;
        pa_context*           ctx      = nullptr;
    };

    void postRedraw(SPulseState* s) {
        MainThread::post([weak = s->weak_from_this()] {
            if (weak.lock() && g_barManager)
                g_barManager->requestRedraw();
        });
    }

    // PA thread
    void onSinkInfo(pa_context*, const pa_sink_info* i, int eol, void* ud) {
        if (eol > 0 || !i)
            return;

        auto* const       S    = static_cast<SPulseState*>(ud);
        const std::string NAME = i->name ? i->name : "";

        std::string portRaw;
        if (i->active_port && i->active_port->name)
            portRaw = i->active_port->name;
        else if (i->active_port && i->active_port->description)
            portRaw = i->active_port->description;
        std::ranges::transform(portRaw, portRaw.begin(), [](unsigned char c) { return (char)std::tolower(c); });

        std::string kind = "default";
        if (portRaw.contains("headphone"))
            kind = "headphone";
        else if (portRaw.contains("speaker"))
            kind = "speaker";
        else if (portRaw.contains("hdmi"))
            kind = "hdmi";

        bool bt = NAME.starts_with("bluez_");
        if (!bt && i->proplist) {
            const char* API = pa_proplist_gets(i->proplist, PA_PROP_DEVICE_API);
            const char* BUS = pa_proplist_gets(i->proplist, PA_PROP_DEVICE_BUS);
            bt = (API && std::string_view{API} == "bluez5") || (BUS && std::string_view{BUS} == "bluetooth");
        }

        {
            std::lock_guard lk(S->mtx);
            if (S->teardown)
                return;
            S->volumePct = (int)std::lround(pa_cvolume_avg(&i->volume) * 100.0 / PA_VOLUME_NORM);
            S->muted     = i->mute;
            S->bluetooth = bt;
            S->portKind  = kind;
            S->desc      = i->description ? i->description : "";
            S->sinkName  = NAME;
            S->cv        = i->volume;
            S->ready     = true;
        }
        postRedraw(S);
    }

    // PA thread
    void onServerInfo(pa_context* c, const pa_server_info* i, void* ud) {
        auto* const S = static_cast<SPulseState*>(ud);

        if (!i || !i->default_sink_name || !*i->default_sink_name) {
            bool wasReady = false;
            {
                std::lock_guard lk(S->mtx);
                if (S->teardown)
                    return;
                wasReady = S->ready;
                S->ready = false;
            }
            if (wasReady)
                postRedraw(S);
            return;
        }

        {
            std::lock_guard lk(S->mtx);
            if (S->teardown)
                return;
        }
        if (auto* const OP = pa_context_get_sink_info_by_name(c, i->default_sink_name, onSinkInfo, S))
            pa_operation_unref(OP);
    }

    // PA thread
    void queryState(pa_context* c, SPulseState* s) {
        {
            std::lock_guard lk(s->mtx);
            if (s->teardown)
                return;
        }
        if (auto* const OP = pa_context_get_server_info(c, onServerInfo, s))
            pa_operation_unref(OP);
    }

    // PA thread
    void onSubscribeEvent(pa_context* c, pa_subscription_event_type_t, uint32_t, void* ud) {
        // subscribed to SINK|SERVER only: any event may change the default
        // sink or its volume/mute/port — re-run the full query chain.
        queryState(c, static_cast<SPulseState*>(ud));
    }

    // PA thread
    void onContextState(pa_context* c, void* ud) {
        auto* const S = static_cast<SPulseState*>(ud);

        switch (pa_context_get_state(c)) {
            case PA_CONTEXT_READY: {
                pa_context_set_subscribe_callback(c, onSubscribeEvent, S);
                if (auto* const OP = pa_context_subscribe(c, (pa_subscription_mask_t)(PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER), nullptr, nullptr))
                    pa_operation_unref(OP);
                queryState(c, S);
                break;
            }
            case PA_CONTEXT_FAILED:
            case PA_CONTEXT_TERMINATED: {
                bool wasReady = false;
                {
                    std::lock_guard lk(S->mtx);
                    if (S->teardown)
                        return;
                    wasReady = S->ready;
                    S->ready = false;
                }
                if (wasReady)
                    postRedraw(S);
                break;
            }
            default: break;
        }
    }

    class CPulseModule : public IModule {
      public:
        explicit CPulseModule(const SModuleConfig& cfg) : IModule(cfg), m_state(std::make_shared<SPulseState>()) {}

        ~CPulseModule() override {
            {
                std::lock_guard lk(m_state->mtx);
                m_state->teardown = true;
            }
            // teardown order matters: callbacks cleared + context dropped under
            // the mainloop lock (blocks until in-flight dispatch finishes and
            // cancels pending operation callbacks), only then stop + free.
            if (m_state->mainloop) {
                pa_threaded_mainloop_lock(m_state->mainloop);
                if (m_state->ctx) {
                    pa_context_set_state_callback(m_state->ctx, nullptr, nullptr);
                    pa_context_set_subscribe_callback(m_state->ctx, nullptr, nullptr);
                    pa_context_disconnect(m_state->ctx);
                    pa_context_unref(m_state->ctx);
                    m_state->ctx = nullptr;
                }
                pa_threaded_mainloop_unlock(m_state->mainloop);
                pa_threaded_mainloop_stop(m_state->mainloop);
                pa_threaded_mainloop_free(m_state->mainloop);
                m_state->mainloop = nullptr;
            } else if (m_state->ctx) {
                pa_context_set_state_callback(m_state->ctx, nullptr, nullptr);
                pa_context_disconnect(m_state->ctx);
                pa_context_unref(m_state->ctx);
                m_state->ctx = nullptr;
            }
        }

        void init() override {
            m_state->mainloop = pa_threaded_mainloop_new();
            if (!m_state->mainloop)
                return;

            auto* const API = pa_threaded_mainloop_get_api(m_state->mainloop);
            m_state->ctx    = pa_context_new(API, "hyprstatus");
            if (!m_state->ctx) {
                pa_threaded_mainloop_free(m_state->mainloop);
                m_state->mainloop = nullptr;
                return;
            }

            pa_context_set_state_callback(m_state->ctx, onContextState, m_state.get());
            // NOAUTOSPAWN: never fork a daemon inside the compositor.
            // NOFAIL: libpulse retries internally if pipewire-pulse is down.
            pa_context_connect(m_state->ctx, nullptr, (pa_context_flags_t)(PA_CONTEXT_NOAUTOSPAWN | PA_CONTEXT_NOFAIL), nullptr);
            pa_threaded_mainloop_start(m_state->mainloop);
        }

        std::vector<SSegment> segments(PHLMONITOR) override {
            int         volume = 0;
            bool        muted = false, bt = false, ready = false;
            std::string kind, desc;
            {
                std::lock_guard lk(m_state->mtx);
                ready  = m_state->ready;
                volume = m_state->volumePct;
                muted  = m_state->muted;
                bt     = m_state->bluetooth;
                kind   = m_state->portKind;
                desc   = m_state->desc;
            }
            if (!ready)
                return {};

            const auto  DEFAULTFMT = opt("format", "{icon} {volume}%");
            std::string fmt        = DEFAULTFMT;
            std::string cls;
            if (muted) {
                fmt = opt("format-muted", DEFAULTFMT);
                cls = "muted";
            } else if (bt)
                fmt = opt("format-bluetooth", DEFAULTFMT);

            const auto ICON = opt("format-icons." + kind, opt("format-icons.default", ""));
            const auto VOL  = std::to_string(volume);
            Hyprutils::String::replaceInString(fmt, "{icon}", ICON);
            Hyprutils::String::replaceInString(fmt, "{volume}", VOL);
            Hyprutils::String::replaceInString(fmt, "{desc}", desc);

            SSegment seg;
            seg.text = fmt;
            seg.cls  = cls;
            if (auto tip = opt("tooltip-format"); !tip.empty()) {
                Hyprutils::String::replaceInString(tip, "{icon}", ICON);
                Hyprutils::String::replaceInString(tip, "{volume}", VOL);
                Hyprutils::String::replaceInString(tip, "{desc}", desc);
                seg.tooltip = tip;
            }
            return {seg};
        }

        bool hidden(PHLMONITOR) override {
            std::lock_guard lk(m_state->mtx);
            return !m_state->ready;
        }

        void onScroll(double delta, const SSegment& seg, PHLMONITOR mon) override {
            if (hasOpt("on-scroll-up") || hasOpt("on-scroll-down")) {
                IModule::onScroll(delta, seg, mon);
                return;
            }

            const int64_t STEP = optInt("scroll-step", 5);
            const int64_t MAX  = optInt("max-volume", 100);

            pa_cvolume  cv{};
            std::string sink;
            int64_t     current = 0;
            {
                // never hold m_state->mtx across the mainloop lock: the PA
                // thread takes mtx while holding the mainloop lock (deadlock).
                std::lock_guard lk(m_state->mtx);
                if (!m_state->ready || m_state->sinkName.empty() || m_state->cv.channels == 0)
                    return;
                cv      = m_state->cv;
                sink    = m_state->sinkName;
                current = m_state->volumePct;
            }
            const auto TARGET = std::clamp(current + (delta < 0 ? STEP : -STEP), (int64_t)0, MAX);
            pa_cvolume_scale(&cv, (pa_volume_t)(TARGET * (double)PA_VOLUME_NORM / 100.0));

            if (!m_state->mainloop || !m_state->ctx)
                return;
            pa_threaded_mainloop_lock(m_state->mainloop);
            if (auto* const OP = pa_context_set_sink_volume_by_name(m_state->ctx, sink.c_str(), &cv, nullptr, nullptr))
                pa_operation_unref(OP);
            pa_threaded_mainloop_unlock(m_state->mainloop);
        }

        void onClick(uint32_t button, const SSegment& seg, PHLMONITOR mon) override {
            const char* KEY = button == BTN_RIGHT ? "on-click-right" : button == BTN_MIDDLE ? "on-click-middle" : "on-click";
            if (hasOpt(KEY)) {
                IModule::onClick(button, seg, mon);
                return;
            }
            if (button != BTN_MIDDLE)
                return;

            bool        muted = false;
            std::string sink;
            {
                std::lock_guard lk(m_state->mtx);
                if (!m_state->ready || m_state->sinkName.empty())
                    return;
                muted = m_state->muted;
                sink  = m_state->sinkName;
            }

            if (!m_state->mainloop || !m_state->ctx)
                return;
            pa_threaded_mainloop_lock(m_state->mainloop);
            if (auto* const OP = pa_context_set_sink_mute_by_name(m_state->ctx, sink.c_str(), muted ? 0 : 1, nullptr, nullptr))
                pa_operation_unref(OP);
            pa_threaded_mainloop_unlock(m_state->mainloop);
        }

      private:
        std::shared_ptr<SPulseState> m_state;
    };

} // namespace

UP<IModule> makePulseModule(const SModuleConfig& cfg) {
    return makeUnique<CPulseModule>(cfg);
}
