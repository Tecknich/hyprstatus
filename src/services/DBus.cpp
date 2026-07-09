#include "DBus.hpp"

#define WLR_USE_UNSTABLE
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>

#include <wayland-server-core.h>

#include <chrono>

namespace {
    struct SBusConn {
        sd_bus*          bus    = nullptr;
        wl_event_source* source = nullptr;
    };

    SBusConn            g_session;
    SBusConn            g_system;
    SP<CEventLoopTimer> g_pumpTimer;
    bool                g_dead = false; // shutdown() ran: no reconnects

    void disconnect(SBusConn& conn, bool flushClose) {
        if (conn.source) {
            wl_event_source_remove(conn.source);
            conn.source = nullptr;
        }
        if (conn.bus) {
            if (flushClose)
                sd_bus_flush_close_unref(conn.bus);
            else
                sd_bus_unref(conn.bus);
            conn.bus = nullptr;
        }
    }

    void pump(SBusConn& conn) {
        if (!conn.bus)
            return;
        int ret = 0;
        while ((ret = sd_bus_process(conn.bus, nullptr)) > 0) {}
        if (ret < 0)
            disconnect(conn, false); // broken connection; next session()/system() call reconnects
    }

    int onBusReadable(int /*fd*/, uint32_t /*mask*/, void* data) {
        pump(*static_cast<SBusConn*>(data));
        return 0;
    }

    void ensurePumpTimer() {
        if (g_pumpTimer)
            return;
        // coarse periodic pump for sd-bus timeouts + stray outgoing queues,
        // shared by both buses (avoids full sd_bus_get_timeout integration)
        g_pumpTimer = makeShared<CEventLoopTimer>(
            std::chrono::seconds(1),
            [](SP<CEventLoopTimer> self, void*) {
                pump(g_session);
                pump(g_system);
                self->updateTimeout(std::chrono::seconds(1)); // timers are one-shot: rearm
            },
            nullptr);
        g_pEventLoopManager->addTimer(g_pumpTimer);
    }

    sd_bus* connect(SBusConn& conn, bool systemBus) {
        if (conn.bus)
            return conn.bus;
        if (g_dead)
            return nullptr;

        sd_bus*   bus = nullptr;
        const int RET = systemBus ? sd_bus_default_system(&bus) : sd_bus_default_user(&bus);
        if (RET < 0 || !bus)
            return nullptr;

        const int FD = sd_bus_get_fd(bus);
        if (FD < 0) {
            sd_bus_unref(bus);
            return nullptr;
        }

        conn.source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, FD, WL_EVENT_READABLE, onBusReadable, &conn);
        if (!conn.source) {
            sd_bus_unref(bus);
            return nullptr;
        }

        conn.bus = bus;
        ensurePumpTimer();
        return conn.bus;
    }
}

sd_bus* DBus::session() {
    return connect(g_session, false);
}

sd_bus* DBus::system() {
    return connect(g_system, true);
}

void DBus::flush(sd_bus* bus) {
    if (!bus)
        return;
    SBusConn* conn = bus == g_session.bus ? &g_session : bus == g_system.bus ? &g_system : nullptr;
    if (!conn)
        return;
    pump(*conn);
    if (conn->bus) // pump may have dropped a broken connection
        sd_bus_flush(conn->bus);
}

void DBus::shutdown() {
    g_dead = true;
    if (g_pumpTimer) {
        g_pEventLoopManager->removeTimer(g_pumpTimer);
        g_pumpTimer.reset();
    }
    disconnect(g_session, true);
    disconnect(g_system, true);
}
