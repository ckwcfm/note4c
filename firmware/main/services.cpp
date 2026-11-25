#include "services.hpp"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <array>
#include <cstring>

namespace note4c::idf {
bool NvsSettings::init() {
    const auto result=nvs_flash_init();
    ready_=result==ESP_OK;
    if (!ready_) ESP_LOGE("settings","NVS unavailable: %s; storage preserved",esp_err_to_name(result));
    return ready_;
}
LoadResult NvsSettings::load(Settings& settings) {
    if (!ready_) return LoadResult::Error;
    nvs_handle_t handle{};
    auto result=nvs_open("note4c_fw",NVS_READONLY,&handle);
    if (result==ESP_ERR_NVS_NOT_FOUND) return LoadResult::Missing;
    if (result!=ESP_OK) return LoadResult::Error;
    std::array<uint8_t,SettingsRecord::size> record{}; size_t size=record.size();
    result=nvs_get_blob(handle,"settings",record.data(),&size); nvs_close(handle);
    if (result==ESP_ERR_NVS_NOT_FOUND) return LoadResult::Missing;
    if (result==ESP_ERR_NVS_INVALID_LENGTH) return LoadResult::Incompatible;
    if (result!=ESP_OK) return LoadResult::Error;
    return SettingsRecord::decode(record.data(),size,settings) ?
        LoadResult::Ok : LoadResult::Incompatible;
}
bool NvsSettings::save(const Settings& settings) {
    if (!ready_) return false;
    const auto record=SettingsRecord::encode(settings);
    nvs_handle_t handle{};
    if (nvs_open("note4c_fw",NVS_READWRITE,&handle)!=ESP_OK) return false;
    auto result=nvs_set_blob(handle,"settings",record.data(),record.size());
    if (result==ESP_OK) result=nvs_commit(handle);
    nvs_close(handle);
    return result==ESP_OK;
}
bool Wifi::init() {
    if (initialized_) return true;
    auto result=esp_netif_init();
    if (result!=ESP_OK) return false;
    result=esp_event_loop_create_default();
    if (result!=ESP_OK && result!=ESP_ERR_INVALID_STATE) return false;
    netif_=esp_netif_create_default_wifi_sta();
    if (!netif_) return false;
    wifi_init_config_t config=WIFI_INIT_CONFIG_DEFAULT();
    result=esp_wifi_init(&config);
    if (result!=ESP_OK) { esp_netif_destroy_default_wifi(netif_); netif_=nullptr; return false; }
    // Do not persist credentials through the Wi-Fi driver's default flash storage.
    result=esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result==ESP_OK) result=esp_event_handler_instance_register(
        WIFI_EVENT,ESP_EVENT_ANY_ID,event,this,&wifi_events_);
    if (result==ESP_OK) result=esp_event_handler_instance_register(
        IP_EVENT,IP_EVENT_STA_GOT_IP,event,this,&ip_events_);
    if (result!=ESP_OK) {
        if (wifi_events_) esp_event_handler_instance_unregister(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_events_);
        esp_wifi_deinit(); esp_netif_destroy_default_wifi(netif_); netif_=nullptr;
        wifi_events_=nullptr; return false;
    }
    initialized_=true; return true;
}
bool Wifi::connect(std::string_view ssid,std::string_view password) {
    // Caller owns connection policy. Avoid unbounded reconnect loops.
    if (state()!=NetworkState::Off || ssid.empty() || ssid.size()>32 ||
        password.size()>63 || (!password.empty() && password.size()<8)) return false;
    if (!init()) { state_.store(NetworkState::Failed); return false; }
    wifi_config_t config{};
    std::memcpy(config.sta.ssid,ssid.data(),ssid.size());
    if (!password.empty()) std::memcpy(config.sta.password,password.data(),password.size());
    config.sta.threshold.authmode=password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable=true;
    auto result=esp_wifi_set_mode(WIFI_MODE_STA);
    if (result==ESP_OK) result=esp_wifi_set_config(WIFI_IF_STA,&config);
    // Best-effort clear the local copy; no credential values are logged.
    volatile uint8_t* secret=config.sta.password;
    for (size_t i=0;i<sizeof(config.sta.password);++i) secret[i]=0;
    if (result==ESP_OK) {
        enabled_.store(true); state_.store(NetworkState::Connecting);
        result=esp_wifi_start();
    }
    if (result!=ESP_OK) { enabled_.store(false); state_.store(NetworkState::Failed); return false; }
    return true;
}
void Wifi::stop() {
    enabled_.store(false);
    if (!initialized_ || state()==NetworkState::Off) {
        state_.store(NetworkState::Off); return;
    }
    // Wait for the asynchronous stop event before allowing a new connection.
    // This prevents old disconnect/IP events from corrupting the next attempt.
    state_.store(NetworkState::Stopping);
    const auto result=esp_wifi_stop();
    if (result==ESP_ERR_WIFI_NOT_STARTED) state_.store(NetworkState::Off);
    else if (result!=ESP_OK) state_.store(NetworkState::Failed);
}
void Wifi::event(void* context,esp_event_base_t base,int32_t id,void*) {
    auto& self=*static_cast<Wifi*>(context);
    if (base==WIFI_EVENT && id==WIFI_EVENT_STA_STOP) {
        self.state_.store(NetworkState::Off); return;
    }
    if (!self.enabled_.load()) return;
    if (base==WIFI_EVENT && id==WIFI_EVENT_STA_START) {
        if (esp_wifi_connect()!=ESP_OK) self.state_.store(NetworkState::Failed);
    } else if (base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        self.state_.store(NetworkState::Failed);
    } else if (base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        self.state_.store(NetworkState::Connected);
    }
}
} // namespace note4c::idf
