#include "note4c/display.hpp"

namespace note4c {
bool DisplayScheduler::pending() const {
    return have_desired_ && (!have_presented_ || desired_ != presented_);
}
void DisplayScheduler::submit(const Frame& frame, Millis now) {
    if (have_desired_ && desired_ == frame) return;
    const bool was_pending = pending();
    desired_ = frame;
    have_desired_ = true;
    if (!was_pending) first_change_ = now;
    last_change_ = now;
}
void DisplayScheduler::retry(Millis now) {
    if (!faulted_ || busy_) return;
    faulted_ = false;
    first_change_ = last_change_ = now;
}
void DisplayScheduler::tick(Millis now) {
    if (busy_) {
        const auto result = port_.poll();
        if (result == Transfer::Busy) return;
        busy_ = false;
        have_completed_ = true;
        completed_at_ = now;
        if (result == Transfer::Failure) {
            // The physical image may be partially updated: it is now unknown.
            have_presented_ = false;
            faulted_ = true;
            return;
        }
        presented_ = inflight_;
        have_presented_ = true;
        ++completed_;
    }
    if (faulted_ || !pending()) return;
    if (have_completed_ && now - completed_at_ < policy_.cooldown_ms) return;
    if (now - last_change_ < policy_.quiet_ms &&
        now - first_change_ < policy_.max_coalesce_ms) return;
    inflight_ = desired_;
    const auto caps = port_.capabilities();
    if (caps.width != Frame::width || caps.height != Frame::height ||
        !port_.start_full_refresh(inflight_)) {
        faulted_ = true;
        return;
    }
    busy_ = true;
}
} // namespace note4c
