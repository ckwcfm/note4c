#pragma once
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <atomic>
#include <string>
#include <string_view>
#include <vector>

namespace note4c::idf {

struct WifiScanEntry {
    std::string ssid;
    int rssi = -100;
    bool open = false;
};

class WifiPortal {
public:
    static constexpr const char* kApSsid = "NOTE4C-Setup";
    static constexpr const char* kApIp = "192.168.4.1";

    bool connect_sta(std::string_view ssid, std::string_view password, uint32_t timeout_ms = 15000);
    bool start_ap();
    void stop();

    // Scans nearby 2.4 GHz APs (works in both STA and APSTA modes).
    std::vector<WifiScanEntry> scan_networks();

    bool is_sta_connected() const { return sta_connected_.load(std::memory_order_acquire); }
    bool is_ap_active() const { return ap_active_.load(std::memory_order_acquire); }
    std::string ip_address() const;

private:
    bool ensure_init();
    void start_dns_server();
    void stop_dns_server();
    static void dns_task_fn(void* ctx);
    static void event_handler(void* ctx, esp_event_base_t base, int32_t id, void* data);

    bool initialized_ = false;
    esp_netif_t* sta_netif_ = nullptr;
    esp_netif_t* ap_netif_ = nullptr;
    esp_event_handler_instance_t wifi_ev_ = nullptr;
    esp_event_handler_instance_t ip_ev_ = nullptr;
    EventGroupHandle_t events_ = nullptr;

    std::atomic<bool> sta_connected_{false};
    std::atomic<bool> ap_active_{false};
    std::atomic<bool> dns_running_{false};
    TaskHandle_t dns_task_ = nullptr;
    int dns_sock_ = -1;
    std::string ip_str_;
};

} // namespace note4c::idf
