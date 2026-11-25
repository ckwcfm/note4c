#include "cal_storage.hpp"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <vector>

namespace note4c::idf {
namespace {
constexpr const char* kTag = "cal_nvs";
constexpr const char* kNs = "note4c_cal";
constexpr const char* kKey = "config";
} // namespace

bool CalendarStorage::init() {
    const auto err = nvs_flash_init();
    ready_ = (err == ESP_OK);
    if (!ready_) {
        ESP_LOGE(kTag, "NVS unavailable (%s); storage preserved without erase", esp_err_to_name(err));
    }
    return ready_;
}

bool CalendarStorage::load(CalendarConfig& out) {
    if (!ready_) return false;
    nvs_handle_t h{};
    auto err = nvs_open(kNs, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_open read failed: %s", esp_err_to_name(err));
        return false;
    }
    size_t len = 0;
    err = nvs_get_blob(h, kKey, nullptr, &len);
    if (err != ESP_OK || len == 0 || len > 8192) {
        nvs_close(h);
        return false;
    }
    std::vector<uint8_t> buf(len);
    err = nvs_get_blob(h, kKey, buf.data(), &len);
    nvs_close(h);
    if (err != ESP_OK) return false;
    CalendarConfig parsed;
    if (!CalendarConfig::decode(buf.data(), len, parsed)) {
        ESP_LOGW(kTag, "Stored calendar config incompatible; preserving NVS");
        return false;
    }
    out = std::move(parsed);
    return true;
}

bool CalendarStorage::save(const CalendarConfig& cfg) {
    if (!ready_) return false;
    const auto blob = CalendarConfig::encode(cfg);
    nvs_handle_t h{};
    auto err = nvs_open(kNs, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_open write failed: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_blob(h, kKey, blob.data(), blob.size());
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_set_blob failed (%u bytes): %s",
                 static_cast<unsigned>(blob.size()), esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(kTag, "Saved calendar config (%u bytes, %u calendars)",
             static_cast<unsigned>(blob.size()), static_cast<unsigned>(cfg.calendars.size()));
    return true;
}

} // namespace note4c::idf
