#include "DBus.hpp"

#define WLR_USE_UNSTABLE
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>

#include <wayland-server-core.h>

#include <cerrno>
#include <chrono>
#include <functional>
#include <map>
#include <string>

namespace {
    struct SBusConn {
        sd_bus*          bus     = nullptr;
        wl_event_source* source  = nullptr;
        bool             pumping = false; // reentrancy guard: sd_bus forbids recursive processing
        bool             wanted  = false; // a consumer connected this bus at least once: keep reconnecting it
        // fired after a successful RECONNECT (not first connect); keyed by consumer
        // so a module rebuilt on reload replaces its entry instead of leaking one.
        std::map<std::string, std::function<void()>> reconnectCbs;
    };

    SBusConn            g_session;
    SBusConn            g_system;
    SP<CEventLoopTimer> g_pumpTimer;
    bool                g_dead = false; // shutdown() ran: no reconnects

    // forward decls: connect -> ensurePumpTimer -> (timer lambda) maybeReconnect -> connect form a cycle
    sd_bus* connect(SBusConn& conn, bool systemBus);
    void    ensurePumpTimer();
    void    maybeReconnect(SBusConn& conn, bool systemBus);

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
        if (!conn.bus || conn.pumping)
            return; // already dispatching: recursive sd_bus_process is forbidden, nothing to do

        conn.pumping = true;
        int ret      = 0;
        while ((ret = sd_bus_process(conn.bus, nullptr)) > 0) {}
        conn.pumping = false;

        // -EBUSY only means we were entered recursively from within dispatch; it is NOT a broken
        // connection, so never tear down on it. Only real transport errors warrant a disconnect.
        if (ret < 0 && ret != -EBUSY)
            // Broken connection: flush_close_unref CLOSES it (marks it dead) and drops our ref, then
            // null it. The pump timer reopens a fresh private connection on the next tick, and any
            // consumer slot still holding a ref keeps the dead object valid until that slot is unref'd.
            disconnect(conn, true);
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
                // Self-heal first: reopen any bus that a consumer wants but that dropped, then pump.
                maybeReconnect(g_session, false);
                maybeReconnect(g_system, true);
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

        sd_bus* bus = nullptr;
        // PRIVATE connection, NOT sd_bus_default_*: a cached default that errors stays cached and dead
        // forever (re-registering its fd never revives it), so the module would never recover. A private
        // open() always yields a genuinely fresh connection after teardown -> reconnect actually works.
        const int RET = systemBus ? sd_bus_open_system(&bus) : sd_bus_open_user(&bus);
        if (RET < 0 || !bus)
            return nullptr;

        const int FD = sd_bus_get_fd(bus);
        if (FD < 0) {
            sd_bus_unref(bus);
            return nullptr;
        }

        // fd is per-connection: on every (re)open this registration runs afresh with the new fd.
        conn.source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, FD, WL_EVENT_READABLE, onBusReadable, &conn);
        if (!conn.source) {
            sd_bus_unref(bus);
            return nullptr;
        }

        conn.bus    = bus;
        conn.wanted = true; // remember it: the pump timer keeps reopening this bus if it later drops
        ensurePumpTimer();
        return conn.bus;
    }

    // Called every pump tick. If a wanted bus is currently down, reopen it and, on success, notify
    // consumers so they re-establish match slots + refetch on the fresh connection.
    void maybeReconnect(SBusConn& conn, bool systemBus) {
        if (g_dead || conn.bus || !conn.wanted)
            return;
        if (!connect(conn, systemBus))
            return; // still unavailable; retry next tick

        // Copy before firing: a callback may re-enter DBus (e.g. refetch -> system()).
        const auto CBS = conn.reconnectCbs;
        for (const auto& [KEY, CB] : CBS) {
            if (CB)
                CB();
        }
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
    // sd_bus_flush writes the outgoing queue to the socket WITHOUT dispatching, so it is safe to
    // call from inside an sd-bus callback. Do NOT pump() here: sd_bus_process would return -EBUSY
    // when re-entered during dispatch (D-Bus modules call flush() from within their callbacks).
    sd_bus_flush(bus);
}

void DBus::onReconnect(bool systemBus, const std::string& key, std::function<void()> cb) {
    if (g_dead || !cb)
        return;
    // keyed insert/replace: a module re-registering on reload overwrites its own
    // stale entry, so the map is bounded by the number of distinct consumers.
    (systemBus ? g_system : g_session).reconnectCbs[key] = std::move(cb);
}

void DBus::init() {
    // Clear the shutdown latch so a PLUGIN_INIT that re-runs on resident
    // statics (failed dlclose) gets working buses again. After shutdown() the
    // rest of the state is already clean: buses disconnected, callbacks
    // cleared, wanted=false, pump timer reset.
    g_dead = false;
}

void DBus::shutdown() {
    g_dead = true;
    if (g_pumpTimer) {
        g_pEventLoopManager->removeTimer(g_pumpTimer);
        g_pumpTimer.reset();
    }
    // Drop callbacks before disconnecting: nothing must fire after teardown.
    g_session.reconnectCbs.clear();
    g_system.reconnectCbs.clear();
    g_session.wanted = false;
    g_system.wanted  = false;
    disconnect(g_session, true);
    disconnect(g_system, true);
}
