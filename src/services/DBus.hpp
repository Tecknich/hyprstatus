#pragma once
#include <systemd/sd-bus.h>

#include <functional>
#include <string>

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
    // the main thread and are cleared by shutdown().
    //
    // `key` identifies the consumer (e.g. the module name): re-registering with
    // the same key REPLACES the prior callback, so a module rebuilt on every
    // config reload never accumulates stale entries. The callback should still
    // capture a weak liveness guard (a module can outlive a single reconnect
    // cycle only within one instance's life).
    void onReconnect(bool systemBus, const std::string& key, std::function<void()> cb);

    void shutdown(); // PLUGIN_EXIT: remove fd sources, unref buses, drop callbacks
}
