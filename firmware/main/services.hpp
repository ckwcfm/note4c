#pragma once
#include "note4c/services.hpp"
#include "esp_event.h"
#include "esp_netif.h"
#include <atomic>

namespace note4c::idf {
class NvsSettings final : public SettingsStore {
public:
    bool init(); // Failure leaves settings unavailable; never calls nvs_flash_erase.
    LoadResult load(Settings& settings) override;
    bool save(const Settings& settings) override;
private:
    bool ready_=false;
};
class Wifi final : public Network {
public:
    bool connect(std::string_view ssid,std::string_view password) override;
    void stop() override;
    NetworkState state() const override { return state_.load(std::memory_order_acquire); }
private:
    bool init();
    static void event(void* context,esp_event_base_t base,int32_t id,void* data);
    bool initialized_=false;
    std::atomic<bool> enabled_{false};
    std::atomic<NetworkState> state_{NetworkState::Off};
    esp_netif_t* netif_=nullptr;
    esp_event_handler_instance_t wifi_events_=nullptr, ip_events_=nullptr;
};
} // namespace note4c::idf
