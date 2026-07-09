#include "Exec.hpp"

#define WLR_USE_UNSTABLE
#include <hyprland/src/Compositor.hpp>

#include <hyprutils/os/Process.hpp>

#include <wayland-server-core.h>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

// The wl fd source holds a raw SImpl*; safety rests on the invariant that the
// source is ALWAYS removed before the impl can be freed (dtor + finish() both
// do so). weakSelf lets the callback pin the impl for its own duration in case
// a user callback destroys the owning CAsyncProcess mid-dispatch.
struct CAsyncProcess::SImpl {
    std::weak_ptr<SImpl> weakSelf;
    wl_event_source*     source    = nullptr;
    int                  readFd    = -1;
    pid_t                pid       = -1;
    bool                 streaming = false;
    bool                 done      = false;
    std::string          buf;
    OnLine               onLine;
    OnDone               onDone;
};

namespace {
    using SImplPtr = std::shared_ptr<CAsyncProcess::SImpl>;

    void deliverLines(const SImplPtr& impl) {
        size_t pos = 0;
        // re-check done each round: a user callback may destroy the owner
        while (!impl->done && (pos = impl->buf.find('\n')) != std::string::npos) {
            std::string line = impl->buf.substr(0, pos);
            impl->buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (impl->onLine) {
                auto cb = impl->onLine; // copy: cb may reset impl->onLine via dtor
                cb(std::move(line));
            }
        }
    }

    void finish(const SImplPtr& impl) {
        if (impl->done)
            return;
        impl->done = true;

        // tear down BEFORE user callbacks: they may destroy the owner or spawn
        if (impl->source) {
            wl_event_source_remove(impl->source);
            impl->source = nullptr;
        }
        if (impl->readFd >= 0) {
            close(impl->readFd);
            impl->readFd = -1;
        }

        if (impl->streaming) {
            // stream ended: a trailing partial line is complete now
            if (!impl->buf.empty()) {
                std::string line = std::move(impl->buf);
                impl->buf.clear();
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (impl->onLine) {
                    auto cb = impl->onLine;
                    cb(std::move(line));
                }
            }
            if (impl->onDone) {
                auto cb = std::move(impl->onDone);
                impl->onDone = nullptr;
                cb("");
            }
        } else {
            std::string out = std::move(impl->buf);
            impl->buf.clear();
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
                out.pop_back();
            if (impl->onDone) {
                auto cb = std::move(impl->onDone);
                impl->onDone = nullptr;
                cb(std::move(out));
            }
        }
    }

    int onFdEvent(int fd, uint32_t mask, void* data) {
        // source exists => impl alive at entry; keep pins it across user callbacks
        auto keep = static_cast<CAsyncProcess::SImpl*>(data)->weakSelf.lock();
        if (!keep)
            return 0;

        if (mask & WL_EVENT_READABLE) {
            // AsyncDialogBox trick: O_NONBLOCK only for the drain, then restore
            // "otherwise libwayland won't give us a hangup"
            const int FLAGS = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, FLAGS | O_NONBLOCK);
            char    buf[4096];
            ssize_t ret = 0;
            while ((ret = read(fd, buf, sizeof(buf))) > 0)
                keep->buf.append(buf, ret);
            fcntl(fd, F_SETFL, FLAGS);
        }

        if (keep->streaming)
            deliverLines(keep);

        if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
            finish(keep);

        return 0;
    }

    SImplPtr createImpl(const std::string& shellCmd, bool streaming, CAsyncProcess::OnLine onLine, CAsyncProcess::OnDone onDone) {
        int pfd[2] = {-1, -1};
        // O_CLOEXEC is safe: the grandchild's dup2 to fd 1 clears it
        if (pipe2(pfd, O_CLOEXEC) < 0)
            return nullptr;

        auto impl       = std::make_shared<CAsyncProcess::SImpl>();
        impl->weakSelf  = impl;
        impl->streaming = streaming;
        impl->onLine    = std::move(onLine);
        impl->onDone    = std::move(onDone);
        impl->readFd    = pfd[0];

        impl->source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, pfd[0], WL_EVENT_READABLE, onFdEvent, impl.get());
        if (!impl->source) {
            close(pfd[0]);
            close(pfd[1]);
            return nullptr;
        }

        Hyprutils::OS::CProcess proc("/bin/sh", {"-c", shellCmd});
        proc.setStdoutFD(pfd[1]);

        if (!proc.runAsync()) {
            wl_event_source_remove(impl->source);
            impl->source = nullptr;
            close(pfd[0]);
            close(pfd[1]);
            return nullptr;
        }
        // CRITICAL: drop the parent's write end or HANGUP/EOF never arrives
        close(pfd[1]);

        impl->pid = proc.pid(); // double-forked grandchild: kill()-able, nothing to reap (SA_NOCLDWAIT)
        return impl;
    }
}

std::unique_ptr<CAsyncProcess> CAsyncProcess::run(const std::string& shellCmd, OnDone onDone) {
    auto impl = createImpl(shellCmd, false, nullptr, std::move(onDone));
    if (!impl)
        return nullptr;
    auto p    = std::unique_ptr<CAsyncProcess>(new CAsyncProcess());
    p->m_impl = std::move(impl);
    return p;
}

std::unique_ptr<CAsyncProcess> CAsyncProcess::stream(const std::string& shellCmd, OnLine onLine, OnDone onExit) {
    auto impl = createImpl(shellCmd, true, std::move(onLine), std::move(onExit));
    if (!impl)
        return nullptr;
    auto p    = std::unique_ptr<CAsyncProcess>(new CAsyncProcess());
    p->m_impl = std::move(impl);
    return p;
}

CAsyncProcess::~CAsyncProcess() {
    if (!m_impl)
        return;
    auto& i = *m_impl;
    if (i.source) {
        wl_event_source_remove(i.source);
        i.source = nullptr;
    }
    if (i.readFd >= 0) {
        close(i.readFd);
        i.readFd = -1;
    }
    // kill any not-yet-finished child regardless of mode: a hung run() child
    // (e.g. wedged nvidia-smi / stalled checkupdates) would otherwise survive
    // owner destruction and keep its Custom module busy forever. The grandchild
    // pid is kill()-able and reaps itself (SA_NOCLDWAIT); a done child has
    // already exited so well-behaved commands are unaffected.
    if (!i.done && i.pid > 0)
        ::kill(i.pid, SIGKILL);
    i.done   = true;
    i.onLine = nullptr;
    i.onDone = nullptr;
}

bool CAsyncProcess::running() const {
    return m_impl && !m_impl->done;
}

pid_t CAsyncProcess::pid() const {
    return m_impl ? m_impl->pid : -1;
}

void CAsyncProcess::kill() {
    if (m_impl && !m_impl->done && m_impl->pid > 0)
        ::kill(m_impl->pid, SIGKILL);
}
