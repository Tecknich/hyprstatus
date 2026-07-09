#pragma once
#include <functional>

// Waybar-style `signal: N` refresh: install a handler for SIGRTMIN+N whose
// only action is an async-signal-safe write to a self-pipe registered on the
// compositor event loop. Handlers are chained-restored on shutdown. RT
// signals are unused by Hyprland itself, so this is safe to claim while the
// plugin is loaded. `pkill -RTMIN+9 Hyprland` then refreshes the subscribed
// module(s).
namespace RtSignals {
    void init();     // PLUGIN_INIT (installs nothing until first subscribe)
    void shutdown(); // restore previous handlers, remove fd source
    // cb runs on the MAIN thread when SIGRTMIN+n arrives
    void subscribe(int n, std::function<void()> cb);
    void unsubscribeAll(); // module rebuild
}
