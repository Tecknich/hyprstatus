#include "Signals.hpp"

#define WLR_USE_UNSTABLE
#include <hyprland/src/Compositor.hpp>

#include <wayland-server-core.h>

#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <map>
#include <vector>

namespace {
    // [0] read (wl source), [1] write (signal handler). Created lazily on the
    // first subscribe; kept across unsubscribeAll(), closed only in shutdown().
    int              g_pipe[2] = {-1, -1};
    wl_event_source* g_source  = nullptr;

    std::map<int, std::vector<std::function<void()>>> g_subs;   // n -> callbacks
    std::map<int, struct sigaction>                   g_savedActions;

    void handler(int signo) {
        // async-signal-safe only: one write, errno preserved
        const int SAVED = errno;
        const unsigned char B = static_cast<unsigned char>(signo - SIGRTMIN);
        if (g_pipe[1] >= 0) {
            ssize_t ret = write(g_pipe[1], &B, 1);
            (void)ret; // pipe full = wakeup already pending; drop is fine
        }
        errno = SAVED;
    }

    int onSignalReadable(int fd, uint32_t /*mask*/, void* /*data*/) {
        unsigned char buf[64];
        ssize_t       ret = 0;
        while ((ret = read(fd, buf, sizeof(buf))) > 0) { // O_NONBLOCK; drain (level-triggered)
            for (ssize_t i = 0; i < ret; ++i) {
                const auto IT = g_subs.find(static_cast<int>(buf[i]));
                if (IT == g_subs.end())
                    continue;
                const auto CBS = IT->second; // copy: a cb may (un)subscribe
                for (const auto& cb : CBS) {
                    if (cb)
                        cb();
                }
            }
        }
        return 0;
    }

    bool ensurePipe() {
        if (g_pipe[0] >= 0)
            return true;
        if (pipe2(g_pipe, O_CLOEXEC | O_NONBLOCK) < 0) {
            g_pipe[0] = g_pipe[1] = -1;
            return false;
        }
        g_source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, g_pipe[0], WL_EVENT_READABLE, onSignalReadable, nullptr);
        if (!g_source) {
            close(g_pipe[0]);
            close(g_pipe[1]);
            g_pipe[0] = g_pipe[1] = -1;
            return false;
        }
        return true;
    }
}

void RtSignals::init() {
    // intentionally empty: pipe + source are created on first subscribe
}

void RtSignals::subscribe(int n, std::function<void()> cb) {
    if (n < 0 || n > SIGRTMAX - SIGRTMIN)
        return;
    if (!ensurePipe())
        return;

    auto& vec = g_subs[n];
    if (vec.empty()) { // first cb for this signo: claim it, saving the old action
        struct sigaction sa  = {};
        struct sigaction old = {};
        sa.sa_handler        = handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        if (sigaction(SIGRTMIN + n, &sa, &old) != 0) {
            g_subs.erase(n);
            return;
        }
        g_savedActions[n] = old;
    }
    vec.emplace_back(std::move(cb));
}

void RtSignals::unsubscribeAll() {
    for (auto& [n, old] : g_savedActions)
        sigaction(SIGRTMIN + n, &old, nullptr);
    g_savedActions.clear();
    g_subs.clear();
    // pipe + wl source stay for the next subscribe
}

void RtSignals::shutdown() {
    unsubscribeAll();
    if (g_source) {
        wl_event_source_remove(g_source);
        g_source = nullptr;
    }
    // handlers restored above, so nothing writes anymore; close write end last
    // anyway and the handler's fd check guards a racing in-flight signal
    if (g_pipe[0] >= 0) {
        close(g_pipe[0]);
        g_pipe[0] = -1;
    }
    if (g_pipe[1] >= 0) {
        close(g_pipe[1]);
        g_pipe[1] = -1;
    }
}
