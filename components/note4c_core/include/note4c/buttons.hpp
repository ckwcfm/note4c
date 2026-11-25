#pragma once
#include "note4c/display.hpp"
#include <array>
#include <optional>

namespace note4c {
enum class Button : uint8_t { Up, Down, Confirm };
enum class Press : uint8_t { Short, Long };
struct ButtonEvent { Button button; Press press; };

// Called by the single input owner using active=true for an electrically low pin.
// Held-at-boot buttons are ignored until released. No auto-repeat or double-click.
class Buttons {
public:
    Buttons(Millis debounce_ms = 30, Millis long_ms = 900)
        : debounce_(debounce_ms), long_(long_ms) {}
    std::optional<ButtonEvent> sample(Button button, bool active, Millis now);
    bool held() const;
private:
    struct State {
        bool initialized = false, armed = false, raw = false, stable = false;
        bool long_sent = false;
        Millis changed = 0, pressed = 0;
    };
    std::array<State, 3> state_{};
    Millis debounce_, long_;
};
} // namespace note4c
