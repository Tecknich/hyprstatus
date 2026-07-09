#pragma once
#include <systemd/sd-bus.h>

#include <functional>

// Shared sd-bus connections pumped on the compositor's wayland event loop
// (fd readable -> sd_bus_process loop) plus a coarse periodic pump for
// timeouts/outgoing queues. Main thread only.
//
// Connections are PRIVATE (sd_bus_open_*), not the process-cached
// sd_bus_default_*: a cached default that hits a transport error stays cached
// and dead forever, so a lost bus could never be revived. Private opens yield a
// genuinely fresh connection after teardown, which is what lets the pump timer
// self-heal a wedged bus. See DBus.cpp.
namespace DBus {
    // Lazily connected on first use; nullptr if the bus is unavailable.
    sd_bus* session();
    sd_bus* system();

    // Call after queueing outgoing messages outside a pump cycle
    // (sd_bus_call_async, set_property, emits...) to make sure they flush.
    void flush(sd_bus* bus);

    // Register a callback fired AFTER the given bus successfully RECONNECTS
    // (never on the first connect). The fd and bus object change on reconnect,
    // so any match slots a consumer added referenced the now-dead bus: use this
    // to unref+re-add them on the fresh bus and refetch state. Callbacks run on
    // the main thread, are kept for the plugin's lifetime, and are cleared by
    // shutdown(). There is no unregister, so a consumer whose lifetime is
    // shorter than the plugin (e.g. a module rebuilt on config reload) MUST
    // capture a weak liveness guard and no-op when it has been destroyed.
    void onReconnect(bool systemBus, std::function<void()> cb);

    void shutdown(); // PLUGIN_EXIT: remove fd sources, unref buses, drop callbacks
}
