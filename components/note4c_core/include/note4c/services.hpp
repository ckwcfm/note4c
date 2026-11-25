#pragma once
#include "note4c/display.hpp"
#include <limits>
#include <string_view>

namespace note4c {
struct Settings {
    static constexpr uint32_t schema = 1;
    int32_t counter = 0;
    bool operator==(const Settings& o) const { return counter == o.counter; }
    bool operator!=(const Settings& o) const { return !(*this == o); }
};
enum class LoadResult { Ok, Missing, Incompatible, Error };
// Stable storage representation: version and signed counter as two LE32 fields.
// Decode validates before changing output, so incompatible records remain intact.
struct SettingsRecord {
    static constexpr size_t size = 8;
    static std::array<uint8_t, size> encode(const Settings& settings) {
        std::array<uint8_t, size> out{};
        const uint32_t fields[] = {Settings::schema, static_cast<uint32_t>(settings.counter)};
        for (int f = 0; f < 2; ++f) for (int b = 0; b < 4; ++b)
            out[f * 4 + b] = static_cast<uint8_t>(fields[f] >> (8 * b));
        return out;
    }
    static bool decode(const uint8_t* data, size_t length, Settings& out) {
        if (!data || length != size) return false;
        uint32_t fields[2]{};
        for (int f = 0; f < 2; ++f) for (int b = 0; b < 4; ++b)
            fields[f] |= static_cast<uint32_t>(data[f * 4 + b]) << (8 * b);
        if (fields[0] != Settings::schema) return false;
        const uint32_t value = fields[1];
        out.counter = value <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ?
            static_cast<int32_t>(value) : -static_cast<int32_t>(~value) - 1;
        return true;
    }
};
class SettingsStore {
public:
    virtual ~SettingsStore() = default;
    virtual LoadResult load(Settings& settings) = 0;
    virtual bool save(const Settings& settings) = 0;
};
enum class NetworkState { Off, Connecting, Connected, Stopping, Failed };
class Network {
public:
    virtual ~Network() = default;
    // Credentials are runtime input. Implementations must not log them.
    virtual bool connect(std::string_view ssid, std::string_view password) = 0;
    virtual void stop() = 0;
    virtual NetworkState state() const = 0;
};
struct Services { Settings& settings; Network& network; };

struct Activity {
    bool display_busy = false, display_pending = false, display_fault = false;
    bool settings_dirty = false, network_active = false, button_held = false;
    bool charging = false, app_busy = false;
};
class PowerPolicy {
public:
    static bool may_sleep(const Activity& a, Millis now, Millis last_input,
                          Millis idle_ms) {
        return idle_ms > 0 && now >= last_input && now - last_input >= idle_ms &&
            !(a.display_busy || a.display_pending || a.display_fault ||
              a.settings_dirty || a.network_active || a.button_held ||
              a.charging || a.app_busy);
    }
};
} // namespace note4c
