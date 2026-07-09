#pragma once
#include <systemd/sd-bus.h>

// Shared sd-bus connections pumped on the compositor's wayland event loop
// (fd readable -> sd_bus_process loop) plus a coarse periodic pump for
// timeouts/outgoing queues. Main thread only.
namespace DBus {
    // Lazily connected on first use; nullptr if the bus is unavailable.
    sd_bus* session();
    sd_bus* system();

    // Call after queueing outgoing messages outside a pump cycle
    // (sd_bus_call_async, set_property, emits...) to make sure they flush.
    void flush(sd_bus* bus);

    void shutdown(); // PLUGIN_EXIT: remove fd sources, unref buses
}
