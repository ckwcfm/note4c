#include "note4c/buttons.hpp"

namespace note4c {
std::optional<ButtonEvent> Buttons::sample(Button button, bool active, Millis now) {
    const auto index = static_cast<size_t>(button);
    if (index >= state_.size()) return {};
    auto& s = state_[index];
    if (!s.initialized) {
        s.initialized = true;
        s.raw = s.stable = active;
        s.armed = !active;
        s.changed = now;
        return {};
    }
    if (active != s.raw) { s.raw = active; s.changed = now; }
    if (s.raw != s.stable && now - s.changed >= debounce_) {
        s.stable = s.raw;
        if (s.stable) { s.pressed = now; s.long_sent = false; }
        else if (!s.armed) { s.armed = true; }
        else if (!s.long_sent) { return ButtonEvent{button, Press::Short}; }
    }
    if (s.armed && s.stable && s.raw && !s.long_sent && now - s.pressed >= long_) {
        s.long_sent = true;
        return ButtonEvent{button, Press::Long};
    }
    return {};
}
bool Buttons::held() const {
    for (const auto& s : state_) if (s.raw || s.stable) return true;
    return false;
}
} // namespace note4c
