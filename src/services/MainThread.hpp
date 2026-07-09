#pragma once
#include <functional>

// Thread-safe "run this on the compositor main thread" primitive: an eventfd
// registered on the compositor's wayland event loop + a mutex-guarded queue.
// g_pEventLoopManager->doLater is NOT thread-safe — this is the only sanctioned
// cross-thread entry point in this plugin.
namespace MainThread {
    void init();     // PLUGIN_INIT, main thread
    void shutdown(); // PLUGIN_EXIT, main thread, AFTER all worker threads joined
    // Queue fn; wakes the compositor loop. Safe from any thread. After
    // shutdown() this becomes a silent no-op (never crashes).
    void post(std::function<void()> fn);
}
