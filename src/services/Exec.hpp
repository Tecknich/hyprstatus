#pragma once
#include <functional>
#include <memory>
#include <string>

// Async child process with stdout capture, fd-driven on the compositor's
// wayland event loop (CAsyncDialogBox pattern: CProcess::runAsync + pipe +
// wl_event_loop_add_fd). Main thread only. Destroying the object removes the
// fd source, closes the pipe and (for streaming children) SIGKILLs the child.
//
// NOTE: child exit codes are unreliable inside Hyprland (SA_NOCLDWAIT) —
// callers that need success/failure must encode it in the output.
class CAsyncProcess {
  public:
    using OnDone = std::function<void(std::string output)>; // full stdout at exit (trailing \n trimmed)
    using OnLine = std::function<void(std::string line)>;   // one complete line (no \n)

    // run to completion, deliver full stdout once
    static std::unique_ptr<CAsyncProcess> run(const std::string& shellCmd, OnDone onDone);
    // long-lived child, deliver each stdout line as it arrives; onExit fires
    // when the child closes stdout/exits (may be never)
    static std::unique_ptr<CAsyncProcess> stream(const std::string& shellCmd, OnLine onLine, OnDone onExit);

    ~CAsyncProcess();

    bool  running() const;
    pid_t pid() const;
    void  kill(); // SIGKILL the child (no-op if done)

    CAsyncProcess(const CAsyncProcess&)            = delete;
    CAsyncProcess& operator=(const CAsyncProcess&) = delete;

    struct SImpl;

  private:
    CAsyncProcess() = default;
    std::shared_ptr<SImpl> m_impl;
};
