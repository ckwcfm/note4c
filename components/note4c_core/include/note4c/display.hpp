#pragma once
#include "note4c/frame.hpp"
#include <cstdint>

namespace note4c {
using Millis = uint64_t; // Monotonic elapsed time, never wall-clock time.
enum class Transfer { Busy, Success, Failure };
struct DisplayCapabilities {
    int width = Frame::width, height = Frame::height;
    bool partial_refresh = false; // No validated four-color partial waveform.
};
class DisplayPort {
public:
    virtual ~DisplayPort() = default;
    virtual DisplayCapabilities capabilities() const { return {}; }
    // Nonblocking. On true, frame remains immutable until poll completes.
    // On false, the driver MUST retain no reference to frame.
    virtual bool start_full_refresh(const Frame& frame) = 0;
    // Return completion exactly once for the current job; no callbacks into UI.
    virtual Transfer poll() = 0;
};
struct RefreshPolicy {
    Millis quiet_ms = 600;
    Millis max_coalesce_ms = 2000;
    Millis cooldown_ms = 3000;
};
class DisplayScheduler {
public:
    explicit DisplayScheduler(DisplayPort& port, RefreshPolicy policy = {})
        : port_(port), policy_(policy) {}
    void submit(const Frame& frame, Millis now);
    void tick(Millis now);
    void retry(Millis now); // Explicit recovery; never automatically spin on faults.
    bool busy() const { return busy_; }
    bool faulted() const { return faulted_; }
    bool pending() const;
    uint64_t completed() const { return completed_; }
private:
    DisplayPort& port_;
    RefreshPolicy policy_;
    Frame desired_, inflight_, presented_;
    bool have_desired_ = false, have_presented_ = false;
    bool busy_ = false, faulted_ = false, have_completed_ = false;
    Millis first_change_ = 0, last_change_ = 0, completed_at_ = 0;
    uint64_t completed_ = 0;
};
} // namespace note4c
