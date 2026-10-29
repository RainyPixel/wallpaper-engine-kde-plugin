#pragma once

#include <csignal>
#include <sys/resource.h>

// Exercise real short writes/flush failures without filling the disk or relying on permissions.
class ScopedFileSizeLimit {
public:
    explicit ScopedFileSizeLimit(rlim_t bytes) {
        if (getrlimit(RLIMIT_FSIZE, &m_previous) != 0) return;
        auto limit     = m_previous;
        limit.rlim_cur = bytes;
        m_handler      = std::signal(SIGXFSZ, SIG_IGN);
        m_active       = setrlimit(RLIMIT_FSIZE, &limit) == 0;
        if (! m_active) std::signal(SIGXFSZ, m_handler);
    }
    ~ScopedFileSizeLimit() {
        if (! m_active) return;
        setrlimit(RLIMIT_FSIZE, &m_previous);
        std::signal(SIGXFSZ, m_handler);
    }
    bool active() const { return m_active; }

private:
    rlimit m_previous {};
    void (*m_handler)(int) = SIG_DFL;
    bool m_active          = false;
};
