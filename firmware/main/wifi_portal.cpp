#include "wifi_portal.hpp"
#include "esp_log.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include <algorithm>
#include <cstring>
#include <set>

namespace note4c::idf {
namespace {
constexpr const char* kTag = "wifi_portal";
constexpr EventBits_t kBitConnected = BIT0;
constexpr EventBits_t kBitFailed = BIT1;
} // namespace

bool WifiPortal::ensure_init() {
    if (initialized_) return true;
    auto err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
    if (!events_) events_ = xEventGroupCreate();
    if (!events_) return false;

    sta_netif_ = esp_netif_create_default_wifi_sta();
    ap_netif_ = esp_netif_create_default_wifi_ap();
    if (!sta_netif_ || !ap_netif_) return false;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return false;
    // Keep credentials in our own NVS namespace; do not persist in esp_wifi's blob.
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) return false;
    if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            event_handler, this, &wifi_ev_) != ESP_OK) return false;
    if (esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            event_handler, this, &ip_ev_) != ESP_OK) return false;
    initialized_ = true;
    return true;
}

void WifiPortal::event_handler(void* ctx, esp_event_base_t base, int32_t id, void* data) {
    auto& self = *static_cast<WifiPortal*>(ctx);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!self.ap_active_.load(std::memory_order_acquire)) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        self.sta_connected_.store(false, std::memory_order_release);
        if (self.events_) xEventGroupSetBits(self.events_, kBitFailed);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        ESP_LOGI(kTag, "SoftAP active (SSID=%s)", kApSsid);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(kTag, "Client joined SoftAP");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(kTag, "Client left SoftAP");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto* ev = static_cast<const ip_event_got_ip_t*>(data);
        char buf[16]{};
        esp_ip4addr_ntoa(&ev->ip_info.ip, buf, sizeof(buf));
        self.ip_str_ = buf;
        self.sta_connected_.store(true, std::memory_order_release);
        if (self.events_) xEventGroupSetBits(self.events_, kBitConnected);
    }
}

bool WifiPortal::connect_sta(std::string_view ssid, std::string_view password, uint32_t timeout_ms) {
    if (ssid.empty() || ssid.size() > 32 || password.size() > 63 ||
        (!password.empty() && password.size() < 8)) {
        return false;
    }
    if (!ensure_init()) return false;
    stop();

    wifi_config_t cfg{};
    std::memcpy(cfg.sta.ssid, ssid.data(), ssid.size());
    if (!password.empty()) std::memcpy(cfg.sta.password, password.data(), password.size());
    cfg.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable = true;

    xEventGroupClearBits(events_, kBitConnected | kBitFailed);
    sta_connected_.store(false, std::memory_order_release);
    ap_active_.store(false, std::memory_order_release);
    ip_str_.clear();

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    // Clear local password copy
    volatile uint8_t* p = cfg.sta.password;
    for (size_t i = 0; i < sizeof(cfg.sta.password); ++i) p[i] = 0;
    if (err != ESP_OK) return false;

    if (esp_wifi_start() != ESP_OK) return false;

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    int retries = 0;
    while (xTaskGetTickCount() < deadline) {
        const TickType_t rem = deadline - xTaskGetTickCount();
        const EventBits_t bits = xEventGroupWaitBits(events_, kBitConnected | kBitFailed,
                                                     pdTRUE, pdFALSE, rem);
        if (bits & kBitConnected) {
            ESP_LOGI(kTag, "Connected to Wi-Fi, IP=%s", ip_str_.c_str());
            return true;
        }
        if (bits & kBitFailed) {
            if (++retries <= 2 && xTaskGetTickCount() < deadline) {
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_wifi_connect();
            } else {
                break;
            }
        }
    }
    ESP_LOGW(kTag, "Wi-Fi STA connection timed out or failed");
    stop();
    return false;
}

bool WifiPortal::start_ap() {
    if (!ensure_init()) return false;
    stop();

    wifi_config_t ap_cfg{};
    std::strncpy(reinterpret_cast<char*>(ap_cfg.ap.ssid), kApSsid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = static_cast<uint8_t>(std::strlen(kApSsid));
    ap_cfg.ap.channel = 6;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.beacon_interval = 100;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.pmf_cfg.required = false;

    ap_active_.store(true, std::memory_order_release);
    sta_connected_.store(false, std::memory_order_release);
    ip_str_ = kApIp;

    // Use APSTA so the web UI can still scan for available home Wi-Fi networks!
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) { ESP_LOGE(kTag, "set_mode APSTA failed: %s", esp_err_to_name(err)); return false; }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) { ESP_LOGE(kTag, "set_config AP failed: %s", esp_err_to_name(err)); return false; }
    err = esp_wifi_start();
    if (err != ESP_OK) { ESP_LOGE(kTag, "esp_wifi_start failed: %s", esp_err_to_name(err)); return false; }
    esp_wifi_set_ps(WIFI_PS_NONE);

    start_dns_server();
    ESP_LOGI(kTag, "Setup SoftAP started: SSID=%s IP=%s", kApSsid, kApIp);
    return true;
}

void WifiPortal::stop() {
    stop_dns_server();
    if (!initialized_) return;
    esp_wifi_stop();
    sta_connected_.store(false, std::memory_order_release);
    ap_active_.store(false, std::memory_order_release);
    ip_str_.clear();
}

std::string WifiPortal::ip_address() const {
    return ip_str_;
}

std::vector<WifiScanEntry> WifiPortal::scan_networks() {
    std::vector<WifiScanEntry> out;
    if (!initialized_) return out;
    wifi_scan_config_t sc{};
    sc.show_hidden = false;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return out;
    uint16_t count = 0;
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK || count == 0) return out;
    count = std::min<uint16_t>(count, 20);
    std::vector<wifi_ap_record_t> recs(count);
    if (esp_wifi_scan_get_ap_records(&count, recs.data()) != ESP_OK) return out;

    std::sort(recs.begin(), recs.begin() + count, [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
        return a.rssi > b.rssi;
    });
    std::set<std::string> seen;
    for (uint16_t i = 0; i < count; ++i) {
        std::string ssid(reinterpret_cast<const char*>(recs[i].ssid));
        if (ssid.empty() || seen.count(ssid)) continue;
        seen.insert(ssid);
        out.push_back({std::move(ssid), recs[i].rssi, recs[i].authmode == WIFI_AUTH_OPEN});
        if (out.size() >= 12) break;
    }
    return out;
}

void WifiPortal::start_dns_server() {
    if (dns_running_.load(std::memory_order_acquire)) return;
    dns_running_.store(true, std::memory_order_release);
    xTaskCreate(dns_task_fn, "note4c_dns", 3072, this, 2, &dns_task_);
}

void WifiPortal::stop_dns_server() {
    if (!dns_running_.exchange(false, std::memory_order_acq_rel)) return;
    if (dns_sock_ >= 0) {
        close(dns_sock_);
        dns_sock_ = -1;
    }
    for (int i = 0; i < 25 && dns_task_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void WifiPortal::dns_task_fn(void* ctx) {
    auto& self = *static_cast<WifiPortal*>(ctx);
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        self.dns_task_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    self.dns_sock_ = sock;
    struct timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 250000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sock);
        self.dns_sock_ = -1;
        self.dns_task_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    uint8_t buf[512];
    const uint32_t ap_ip = inet_addr(kApIp);
    while (self.dns_running_.load(std::memory_order_acquire)) {
        sockaddr_in client{};
        socklen_t clen = sizeof(client);
        const int len = recvfrom(sock, buf, sizeof(buf) - 16, 0,
                                 reinterpret_cast<sockaddr*>(&client), &clen);
        if (len < 12) continue;
        // Build minimal DNS A-record response pointing to 192.168.4.1
        buf[2] = 0x81; // QR=1, RD=1
        buf[3] = 0x80; // RA=1, RCODE=0
        buf[6] = 0x00; buf[7] = 0x01; // ANCOUNT=1
        buf[8] = 0x00; buf[9] = 0x00;
        buf[10] = 0x00; buf[11] = 0x00;
        int resp_len = len;
        // Pointer to QNAME at offset 12 (0xC00C), TYPE A, CLASS IN, TTL 30s, RDLENGTH 4
        const uint8_t answer[16] = {
            0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x1E, 0x00, 0x04,
            static_cast<uint8_t>(ap_ip & 0xFF),
            static_cast<uint8_t>((ap_ip >> 8) & 0xFF),
            static_cast<uint8_t>((ap_ip >> 16) & 0xFF),
            static_cast<uint8_t>((ap_ip >> 24) & 0xFF),
        };
        std::memcpy(buf + resp_len, answer, sizeof(answer));
        resp_len += static_cast<int>(sizeof(answer));
        sendto(sock, buf, resp_len, 0, reinterpret_cast<sockaddr*>(&client), clen);
    }

    if (self.dns_sock_ >= 0) {
        close(self.dns_sock_);
        self.dns_sock_ = -1;
    }
    self.dns_task_ = nullptr;
    vTaskDelete(nullptr);
}

} // namespace note4c::idf
