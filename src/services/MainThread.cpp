#include "MainThread.hpp"

#define WLR_USE_UNSTABLE
#include <hyprland/src/Compositor.hpp>

#include <wayland-server-core.h>

#include <sys/eventfd.h>
#include <unistd.h>

#include <mutex>
#include <utility>
#include <vector>

namespace {
    int                                g_wakeFd     = -1;
    wl_event_source*                   g_wakeSource = nullptr;
    std::mutex                         g_mutex; // guards g_queue, g_dead, g_wakeFd
    std::vector<std::function<void()>> g_queue;
    bool                               g_dead = true;

    int onWake(int fd, uint32_t /*mask*/, void* /*data*/) {
        // level-triggered epoll: an undrained eventfd busy-loops the compositor
        uint64_t n = 0;
        while (read(fd, &n, sizeof(n)) > 0) {}

        std::vector<std::function<void()>> fns;
        {
            std::lock_guard lk(g_mutex);
            fns.swap(g_queue);
        }
        // run outside the lock: fns may post() again
        for (auto& fn : fns) {
            if (fn)
                fn();
        }
        return 0;
    }
}

void MainThread::init() {
    std::lock_guard lk(g_mutex);
    g_wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (g_wakeFd < 0)
        return; // g_dead stays true; post() no-ops

    g_wakeSource = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, g_wakeFd, WL_EVENT_READABLE, onWake, nullptr);
    if (!g_wakeSource) {
        close(g_wakeFd);
        g_wakeFd = -1;
        return;
    }
    g_dead = false;
}

void MainThread::post(std::function<void()> fn) {
    // the write happens under the mutex so shutdown() can never close the fd
    // between our liveness check and the wakeup (eventfd writes don't block)
    std::lock_guard lk(g_mutex);
    if (g_dead)
        return;
    g_queue.emplace_back(std::move(fn));

    const uint64_t ONE = 1;
    ssize_t        ret = write(g_wakeFd, &ONE, sizeof(ONE));
    (void)ret; // EAGAIN = counter saturated = wakeup already pending
}

void MainThread::shutdown() {
    std::lock_guard lk(g_mutex);
    g_dead = true;
    if (g_wakeSource) {
        wl_event_source_remove(g_wakeSource);
        g_wakeSource = nullptr;
    }
    if (g_wakeFd >= 0) {
        close(g_wakeFd);
        g_wakeFd = -1;
    }
    g_queue.clear();
}
