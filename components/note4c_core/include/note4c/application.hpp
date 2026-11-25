#pragma once
#include "note4c/buttons.hpp"
#include "note4c/services.hpp"
#include <array>

namespace note4c {
class Application {
public:
    virtual ~Application() = default;
    virtual std::string_view id() const = 0;
    virtual void enter(Services&) {}
    virtual void leave(Services&) {}
    virtual void button(ButtonEvent, Services&) = 0;
    virtual void tick(Millis, Services&) {}
    virtual Screen screen(const Services&) const = 0;
    virtual bool busy() const { return false; }
};

// All methods belong to one event-loop task. Drivers communicate with this task
// via atomic state/queues, never by calling app methods from their callbacks.
class Runtime {
public:
    Runtime(SettingsStore& store, Network& network, DisplayScheduler& display)
        : store_(store), services_{settings_, network}, display_(display) {}
    bool add(Application& app); // Registration is closed after begin().
    bool begin(Millis now);
    void button(ButtonEvent event, Millis now);
    void tick(Millis now);
    bool flush_settings();
    bool settings_dirty() const { return settings_ != saved_; }
    bool storage_error() const { return storage_error_; }
    bool storage_writable() const { return writable_; }
    Activity activity(bool held, bool charging) const;
    Millis last_input() const { return last_input_; }
    std::string_view active_id() const;
private:
    SettingsStore& store_;
    Settings settings_{}, saved_{}, observed_{};
    Services services_;
    DisplayScheduler& display_;
    ScreenRenderer renderer_;
    Frame frame_;
    Screen last_screen_{};
    std::array<Application*, 8> apps_{};
    size_t count_ = 0, active_ = 0;
    bool started_ = false, have_screen_ = false, writable_ = true;
    bool storage_error_ = false;
    Millis last_input_ = 0, settings_changed_ = 0, last_save_attempt_ = 0;
};

class CounterApp final : public Application {
public:
    std::string_view id() const override { return "counter"; }
    void button(ButtonEvent event, Services& services) override;
    Screen screen(const Services& services) const override;
};
class StatusApp final : public Application {
public:
    std::string_view id() const override { return "status"; }
    void button(ButtonEvent, Services&) override {}
    Screen screen(const Services& services) const override;
};
} // namespace note4c
