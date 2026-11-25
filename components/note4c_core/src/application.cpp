#include "note4c/application.hpp"
#include <limits>

namespace note4c {
bool Runtime::add(Application& app) {
    if (started_ || count_ == apps_.size()) return false;
    for (size_t i = 0; i < count_; ++i) if (apps_[i]->id() == app.id()) return false;
    apps_[count_++] = &app;
    return true;
}
bool Runtime::begin(Millis now) {
    if (started_ || count_ == 0) return false;
    Settings loaded{};
    const auto result = store_.load(loaded);
    if (result == LoadResult::Ok) settings_ = loaded;
    writable_ = result == LoadResult::Ok || result == LoadResult::Missing;
    storage_error_ = !writable_;
    saved_ = observed_ = settings_;
    started_ = true;
    last_input_ = settings_changed_ = last_save_attempt_ = now;
    apps_[active_]->enter(services_);
    return true;
}
void Runtime::button(ButtonEvent event, Millis now) {
    if (!started_) return;
    last_input_ = now;
    // Navigation is global; long Confirm remains available to applications.
    if (event.button == Button::Confirm && event.press == Press::Short) {
        apps_[active_]->leave(services_);
        active_ = (active_ + 1) % count_;
        apps_[active_]->enter(services_);
    } else {
        apps_[active_]->button(event, services_);
    }
}
bool Runtime::flush_settings() {
    if (!settings_dirty()) return !storage_error_;
    if (!writable_) return false;
    if (!store_.save(settings_)) { storage_error_ = true; return false; }
    saved_ = settings_;
    storage_error_ = false;
    return true;
}
void Runtime::tick(Millis now) {
    if (!started_) return;
    apps_[active_]->tick(now, services_);
    if (settings_ != observed_) {
        observed_ = settings_;
        settings_changed_ = now;
    }
    if (settings_dirty() && writable_ && now - settings_changed_ >= 1000 &&
        now - last_save_attempt_ >= 1000) {
        last_save_attempt_ = now;
        flush_settings();
    }
    auto screen = apps_[active_]->screen(services_);
    if (!have_screen_ || !(screen == last_screen_)) {
        renderer_.draw(screen, frame_);
        display_.submit(frame_, now);
        last_screen_ = screen;
        have_screen_ = true;
    }
    display_.tick(now);
}
Activity Runtime::activity(bool held, bool charging) const {
    return {display_.busy(), display_.pending(), display_.faulted(),
        settings_dirty() || storage_error_, services_.network.state() != NetworkState::Off,
        held, charging, started_ && apps_[active_]->busy()};
}
std::string_view Runtime::active_id() const {
    return started_ ? apps_[active_]->id() : std::string_view{};
}
void CounterApp::button(ButtonEvent e, Services& s) {
    if (e.press == Press::Long && e.button == Button::Confirm) s.settings.counter = 0;
    if (e.press != Press::Short) return;
    if (e.button == Button::Up && s.settings.counter < std::numeric_limits<int32_t>::max())
        ++s.settings.counter;
    if (e.button == Button::Down && s.settings.counter > std::numeric_limits<int32_t>::min())
        --s.settings.counter;
}
Screen CounterApp::screen(const Services& s) const {
    return {"COUNTER", {"VALUE: " + std::to_string(s.settings.counter),
        "UP +1   DOWN -1", "HOLD CONFIRM: RESET", "CONFIRM: NEXT APP",
        "INPUT WORKS WHILE BUSY"}, Color::Red};
}
Screen StatusApp::screen(const Services& s) const {
    const char* state = "OFF";
    switch (s.network.state()) {
    case NetworkState::Connecting: state = "CONNECTING"; break;
    case NetworkState::Connected: state = "CONNECTED"; break;
    case NetworkState::Stopping: state = "STOPPING"; break;
    case NetworkState::Failed: state = "FAILED"; break;
    case NetworkState::Off: break;
    }
    return {"STATUS", {std::string("WI-FI: ") + state, "400 X 300 BWRY",
        "FULL REFRESH ONLY", "CONFIRM: NEXT APP", "NOTE4C FRAMEWORK"}, Color::Yellow};
}
} // namespace note4c
