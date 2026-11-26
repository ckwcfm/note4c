#pragma once
#include "cal_storage.hpp"
#include "github_ota.hpp"
#include "ics_fetcher.hpp"
#include "wifi_portal.hpp"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <atomic>
#include <string>

namespace note4c::idf {

struct WebRuntimeState {
    CalendarConfig config;
    SyncReport last_sync;
    bool has_synced = false;
    bool syncing = false;
    bool is_ap_mode = false;
    std::string mode_label;
    std::string ip_address;
    std::string mac_address;
    std::string fw_version;
    std::string fw_build_date;
    std::string fw_slot;
    std::string status_message;
    int battery_percent = -1;
    uint16_t battery_mv = 0;
    bool charging = false;
    OtaProgress ota_progress;
    std::string ota_subtitle;
    std::string ota_status;
    bool ota_failed = false;
    bool ota_reboot = false;
};

class WebUiServer {
public:
    WebUiServer(CalendarStorage& storage, WifiPortal& wifi, WebRuntimeState& state, SemaphoreHandle_t mutex);
    ~WebUiServer();

    bool start();
    void stop();
    bool running() const { return server_ != nullptr; }

    bool take_sync_request() { return sync_requested_.exchange(false, std::memory_order_acq_rel); }
    bool take_sleep_request() { return sleep_requested_.exchange(false, std::memory_order_acq_rel); }
    bool take_wifi_changed() { return wifi_changed_.exchange(false, std::memory_order_acq_rel); }
    bool take_reboot_request() { return reboot_requested_.exchange(false, std::memory_order_acq_rel); }
    bool take_github_ota_request(std::string& out_repo, std::string& out_url);
    bool ota_in_progress() const {
        return ota_active_.load(std::memory_order_acquire) ||
               state_.ota_progress.active.load(std::memory_order_acquire);
    }
    bool had_http_activity() const { return had_http_activity_.load(std::memory_order_acquire); }
    Millis last_activity_ms() const { return last_activity_ms_.load(std::memory_order_acquire); }
    void touch_activity();

private:
    static esp_err_t handle_index(httpd_req_t* req);
    static esp_err_t handle_get_config(httpd_req_t* req);
    static esp_err_t handle_post_config(httpd_req_t* req);
    static esp_err_t handle_scan(httpd_req_t* req);
    static esp_err_t handle_sync(httpd_req_t* req);
    static esp_err_t handle_status(httpd_req_t* req);
    static esp_err_t handle_sleep(httpd_req_t* req);
    static esp_err_t handle_reboot(httpd_req_t* req);
    static esp_err_t handle_ota(httpd_req_t* req);
    static esp_err_t handle_ota_check(httpd_req_t* req);
    static esp_err_t handle_ota_github(httpd_req_t* req);
    static esp_err_t handle_404(httpd_req_t* req, httpd_err_code_t err);

    std::string build_status_json(bool include_config);

    CalendarStorage& storage_;
    WifiPortal& wifi_;
    WebRuntimeState& state_;
    SemaphoreHandle_t mutex_;
    httpd_handle_t server_ = nullptr;

    std::atomic<bool> sync_requested_{false};
    std::atomic<bool> sleep_requested_{false};
    std::atomic<bool> wifi_changed_{false};
    std::atomic<bool> reboot_requested_{false};
    std::atomic<bool> github_ota_requested_{false};
    std::string gh_ota_repo_;
    std::string gh_ota_url_;
    std::atomic<bool> ota_active_{false};
    std::atomic<bool> had_http_activity_{false};
    std::atomic<Millis> last_activity_ms_{0};
};

} // namespace note4c::idf
