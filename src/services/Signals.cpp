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

    // The write-end fd the async signal handler uses. It is the ONLY shared state
    // the handler touches, and a volatile sig_atomic_t is the only type it may
    // race-read against a main-thread writer without a data race / strict-
    // conformance UB. g_pipe[1] (a plain int) is main-thread-only; the handler
    // must never read it.
    volatile sig_atomic_t g_writeFd = -1;

    std::map<int, std::vector<std::function<void()>>> g_subs;   // n -> callbacks
    std::map<int, struct sigaction>                   g_savedActions;

    void handler(int signo) {
        // async-signal-safe only: one write, errno preserved. Reads ONLY
        // g_writeFd (volatile sig_atomic_t) into a local so the >= 0 check and the
        // write() see the same value; g_pipe[1] is never touched here.
        const int           SAVED = errno;
        const unsigned char B     = static_cast<unsigned char>(signo - SIGRTMIN);
        const int           FD    = g_writeFd;
        if (FD >= 0) {
            ssize_t ret = write(FD, &B, 1);
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
        // Publish the write end to the handler ONLY once the pipe is fully live.
        g_writeFd = g_pipe[1];
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

    // Claim the signal (install our handler + save the original action) only the
    // first time we ever see this signo. g_savedActions membership -- not g_subs
    // emptiness -- is what tracks whether our handler is installed, because
    // unsubscribeAll() now clears g_subs while deliberately leaving the handler in
    // place across reloads. Keying off g_subs.empty() here would let a reload's
    // re-subscribe re-run sigaction() and overwrite the saved action with our own
    // handler, so shutdown() would "restore" our handler instead of SIG_DFL.
    if (g_savedActions.find(n) == g_savedActions.end()) {
        struct sigaction sa  = {};
        struct sigaction old = {};
        sa.sa_handler        = handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        if (sigaction(SIGRTMIN + n, &sa, &old) != 0)
            return;
        g_savedActions[n] = old;
    }
    g_subs[n].emplace_back(std::move(cb));
}

void RtSignals::unsubscribeAll() {
    // Clear the callback map ONLY. We must NOT restore SIG_DFL here: rebuild()
    // calls this and re-subscribes later in the same pass, so restoring would open
    // a window where SIGRTMIN+N has default disposition (= process termination per
    // signal(7)). A racing `pkill -RTMIN+N Hyprland` -- the plugin's own documented
    // refresh mechanism, fired asynchronously by volume/brightness/mail hooks --
    // arriving in that window would kill the whole compositor session. Leaving our
    // self-pipe handler installed keeps stray signals harmless: they just write a
    // byte that onSignalReadable() drains against an empty callback list = no-op.
    // The saved actions, handler, pipe and wl source all persist for the next
    // subscribe; original dispositions are restored only in shutdown().
    g_subs.clear();
}

void RtSignals::shutdown() {
    // Restore the ORIGINAL signal dispositions here and ONLY here (unsubscribeAll()
    // no longer touches them). After this loop no NEW delivery runs our handler.
    for (auto& [n, old] : g_savedActions)
        sigaction(SIGRTMIN + n, &old, nullptr);
    g_savedActions.clear();
    g_subs.clear();

    // Stop draining the read end before we tear down any pipe fd.
    if (g_source) {
        wl_event_source_remove(g_source);
        g_source = nullptr;
    }

    // Write end teardown, TOCTOU-hardened. An RT signal is process-directed and may
    // already be executing handler() on another compositor thread, having passed
    // its `g_writeFd >= 0` check. First publish -1 to g_writeFd so any not-yet-
    // started handler skips the write; then dup2() /dev/null over the real fd so an
    // in-flight write lands harmlessly instead of into a fd that a plain close()
    // could let get recycled (client socket / dmabuf / log) and silently corrupted.
    // Residual risk: a handler preempted between check and write, resumed only after
    // the final close() below, could still hit a recycled fd -- a nanosecond-wide
    // window per plugin unload, accepted (handler stays write()-only, i.e.
    // async-signal-safe).
    if (g_pipe[1] >= 0) {
        const int WFD = g_pipe[1];
        g_writeFd     = -1; // stop the handler BEFORE any fd teardown
        g_pipe[1]     = -1;
        const int NUL = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (NUL >= 0) {
            dup2(NUL, WFD); // atomically points WFD at /dev/null (closes pipe write end)
            close(NUL);
        }
        close(WFD);
    }
    if (g_pipe[0] >= 0) {
        close(g_pipe[0]);
        g_pipe[0] = -1;
    }
}
