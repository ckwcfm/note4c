#include "board.hpp"
#include "cal_storage.hpp"
#include "ics_fetcher.hpp"
#include "web_ui.hpp"
#include "wifi_portal.hpp"
#include "note4c/buttons.hpp"
#include "note4c/calendar_renderer.hpp"
#include "note4c/display.hpp"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "esp_system.h"
#include <atomic>
#include <cstdio>
#include <new>

using namespace note4c;
using namespace note4c::idf;

namespace {
constexpr const char* kTag = "note4c_cal";
constexpr Millis kManageTimeoutMs = 10ULL * 60ULL * 1000ULL;
constexpr Millis kBatteryIdleBeforeSleepMs = 60ULL * 1000ULL;

struct CalendarEngine {
    DisplayScheduler display;
    Frame render_frame;
    CalendarRenderer renderer;

    explicit CalendarEngine(DisplayPort& port)
        : display(port, RefreshPolicy{200, 1000, 1500}) {}
};

enum class NetJobKind : uint8_t { None = 0, Sync, StartManage, ReconnectWifi };

struct NetJob {
    NetJobKind kind = NetJobKind::None;
    bool keep_wifi = false;
    bool force_ap = false;
    std::string detail;
};

struct AppCtx {
    Board* board = nullptr;
    WifiPortal* wifi = nullptr;
    WebUiServer* web = nullptr;
    WebRuntimeState* web_state = nullptr;
    SemaphoreHandle_t mutex = nullptr;
    TaskHandle_t worker_task = nullptr;

    NetJob pending_job{};
    std::atomic<bool> worker_busy{false};
    std::atomic<bool> sync_result_ready{false};
    std::atomic<bool> manage_screen_ready{false};
    std::atomic<bool> in_manage_mode{false};

    bool manage_ap = false;
    std::string manage_ssid;
    std::string manage_url;
    std::string manage_detail;
};

void refresh_battery_locked(AppCtx& app) {
    uint16_t mv = 0;
    uint8_t pct = 0;
    app.web_state->charging = app.board->charging();
    if (app.board->read_battery(mv, pct)) {
        app.web_state->battery_mv = mv;
        app.web_state->battery_percent = static_cast<int>(pct);
    }
}

void execute_sync(AppCtx& app, bool keep_wifi) {
    xSemaphoreTake(app.mutex, portMAX_DELAY);
    const CalendarConfig cfg = app.web_state->config;
    app.web_state->syncing = true;
    app.web_state->status_message = "Syncing calendars...";
    refresh_battery_locked(app);
    xSemaphoreGive(app.mutex);

    bool connected = app.wifi->is_sta_connected();
    if (!connected && cfg.has_wifi()) {
        connected = app.wifi->connect_sta(cfg.wifi_ssid, cfg.wifi_password, 15000);
    }

    SyncReport rep;
    if (!connected) {
        rep.wifi_ok = false;
        rep.time_ok = false;
        rep.error_count = 1;
    } else {
        // Keep the management web UI reachable whenever Wi-Fi STA is connected,
        // even while the e-paper shows the 3-day calendar!
        xSemaphoreTake(app.mutex, portMAX_DELAY);
        app.web_state->is_ap_mode = false;
        app.web_state->mode_label = "Wi-Fi";
        app.web_state->ip_address = app.wifi->ip_address();
        xSemaphoreGive(app.mutex);
        if (!app.web->running()) {
            app.web->start();
        }
        rep = sync_all_calendars(cfg, *app.board);
        rep.wifi_ok = true;
    }

    xSemaphoreTake(app.mutex, portMAX_DELAY);
    refresh_battery_locked(app);
    app.web_state->last_sync = rep;
    app.web_state->has_synced = true;
    app.web_state->syncing = false;
    if (!rep.wifi_ok) {
        app.web_state->status_message = "Could not connect to Wi-Fi.";
    } else if (!rep.time_ok) {
        app.web_state->status_message = "Time sync failed.";
    } else {
        app.web_state->status_message = "Sync complete.";
    }
    xSemaphoreGive(app.mutex);

    (void)keep_wifi; // Wi-Fi & web server stay active until deep sleep on battery
    app.sync_result_ready.store(true, std::memory_order_release);
}

void execute_start_manage(AppCtx& app, bool force_ap, const std::string& detail) {
    app.web->stop();
    xSemaphoreTake(app.mutex, portMAX_DELAY);
    const CalendarConfig cfg = app.web_state->config;
    xSemaphoreGive(app.mutex);

    bool ap_mode = true;
    std::string ssid = WifiPortal::kApSsid;
    std::string url = std::string("http://") + WifiPortal::kApIp;
    std::string note = detail;

    if (!force_ap && cfg.has_wifi()) {
        if (app.wifi->is_sta_connected() ||
            app.wifi->connect_sta(cfg.wifi_ssid, cfg.wifi_password, 15000)) {
            ap_mode = false;
            ssid = cfg.wifi_ssid;
            url = "http://" + app.wifi->ip_address();
        } else {
            note = "Could not join " + cfg.wifi_ssid + "; started setup hotspot";
        }
    }
    if (ap_mode) {
        app.wifi->start_ap();
    }

    xSemaphoreTake(app.mutex, portMAX_DELAY);
    refresh_battery_locked(app);
    app.web_state->is_ap_mode = ap_mode;
    app.web_state->mode_label = ap_mode ? "Setup AP" : "Wi-Fi";
    app.web_state->ip_address = app.wifi->ip_address();
    app.web_state->status_message = note.empty() ? "Ready." : note;
    app.manage_ap = ap_mode;
    app.manage_ssid = ssid;
    app.manage_url = url;
    app.manage_detail = note;
    xSemaphoreGive(app.mutex);

    app.web->start();
    app.in_manage_mode.store(true, std::memory_order_release);
    app.manage_screen_ready.store(true, std::memory_order_release);
}

void execute_reconnect_wifi(AppCtx& app) {
    // Allow browser to finish receiving the HTTP 200 JSON response first
    vTaskDelay(pdMS_TO_TICKS(900));
    app.web->stop();

    xSemaphoreTake(app.mutex, portMAX_DELAY);
    const CalendarConfig cfg = app.web_state->config;
    xSemaphoreGive(app.mutex);

    if (cfg.has_wifi() && app.wifi->connect_sta(cfg.wifi_ssid, cfg.wifi_password, 15000)) {
        xSemaphoreTake(app.mutex, portMAX_DELAY);
        refresh_battery_locked(app);
        app.web_state->is_ap_mode = false;
        app.web_state->mode_label = "Wi-Fi";
        app.web_state->ip_address = app.wifi->ip_address();
        app.manage_ap = false;
        app.manage_ssid = cfg.wifi_ssid;
        app.manage_url = "http://" + app.wifi->ip_address();
        app.manage_detail = "Wi-Fi connected! Add calendars at this URL.";
        xSemaphoreGive(app.mutex);

        app.web->start();
        if (!cfg.calendars.empty()) {
            app.in_manage_mode.store(false, std::memory_order_release);
            execute_sync(app, true);
        } else {
            app.in_manage_mode.store(true, std::memory_order_release);
            app.manage_screen_ready.store(true, std::memory_order_release);
        }
    } else {
        execute_start_manage(app, true, "Wi-Fi failed; verify SSID & password");
    }
}

void net_worker_task(void* arg) {
    auto& app = *static_cast<AppCtx*>(arg);
    for (;;) {
        xTaskNotifyWait(0, 0xFFFFFFFFu, nullptr, portMAX_DELAY);
        for (;;) {
            xSemaphoreTake(app.mutex, portMAX_DELAY);
            const NetJob job = app.pending_job;
            app.pending_job = NetJob{};
            xSemaphoreGive(app.mutex);
            if (job.kind == NetJobKind::None) break;

            app.worker_busy.store(true, std::memory_order_release);
            if (job.kind == NetJobKind::Sync) {
                execute_sync(app, job.keep_wifi);
            } else if (job.kind == NetJobKind::StartManage) {
                execute_start_manage(app, job.force_ap, job.detail);
            } else if (job.kind == NetJobKind::ReconnectWifi) {
                execute_reconnect_wifi(app);
            }
            app.worker_busy.store(false, std::memory_order_release);
        }
    }
}

void enqueue_job(AppCtx& app, NetJob job) {
    xSemaphoreTake(app.mutex, portMAX_DELAY);
    app.pending_job = std::move(job);
    xSemaphoreGive(app.mutex);
    if (app.worker_task) xTaskNotifyGive(app.worker_task);
}

bool has_pending_job(AppCtx& app) {
    xSemaphoreTake(app.mutex, portMAX_DELAY);
    const bool p = (app.pending_job.kind != NetJobKind::None);
    xSemaphoreGive(app.mutex);
    return p;
}

} // namespace

extern "C" void app_main() {
    static Board board;
    board.init();

    static CalendarStorage storage;
    storage.init();

    static Display port;
    port.init();

    void* mem = heap_caps_malloc(sizeof(CalendarEngine), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) {
        ESP_LOGE(kTag, "PSRAM allocation failed; panel remains off");
        return;
    }
    auto& engine = *new (mem) CalendarEngine(port);
    esp_ota_mark_app_valid_cancel_rollback();

    const esp_partition_t* running_part = esp_ota_get_running_partition();
    const esp_app_desc_t* app_desc = esp_app_get_description();

    ESP_LOGI(kTag, "NOTE4C Calendar starting (%s, slot=%s); PSRAM=%u bytes",
             app_desc ? app_desc->version : "?",
             running_part ? running_part->label : "?",
             static_cast<unsigned>(esp_psram_get_size()));

    SemaphoreHandle_t state_mutex = xSemaphoreCreateMutex();
    static WebRuntimeState web_state;
    web_state.mac_address = board.sta_mac();
    web_state.fw_version = app_desc ? (std::string(app_desc->version) + " " + app_desc->date) : "0.1.0";
    web_state.fw_slot = running_part ? running_part->label : "unknown";
    const bool loaded = storage.load(web_state.config);
    {
        uint16_t mv = 0;
        uint8_t pct = 0;
        web_state.charging = board.charging();
        if (board.read_battery(mv, pct)) {
            web_state.battery_mv = mv;
            web_state.battery_percent = static_cast<int>(pct);
        }
    }
    ESP_LOGI(kTag, "Config loaded=%d, mac=%s, bat=%d%% (%umV), wifi=%s, cals=%u, interval=%um",
             loaded ? 1 : 0,
             web_state.mac_address.c_str(),
             web_state.battery_percent,
             static_cast<unsigned>(web_state.battery_mv),
             web_state.config.has_wifi() ? web_state.config.wifi_ssid.c_str() : "(none)",
             static_cast<unsigned>(web_state.config.calendars.size()),
             static_cast<unsigned>(web_state.config.sync_interval_minutes));

    static WifiPortal wifi;
    static WebUiServer web(storage, wifi, web_state, state_mutex);
    static AppCtx app;
    app.board = &board;
    app.wifi = &wifi;
    app.web = &web;
    app.web_state = &web_state;
    app.mutex = state_mutex;

    const BaseType_t created = xTaskCreate(net_worker_task, "note4c_sync", 12288, &app, 2, &app.worker_task);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    uint32_t next_sleep_sec = 24 * 3600;
    bool synced_this_boot = false;
    Millis idle_since_ms = now_ms();
    Millis last_sync_ms = now_ms();

    const WakeReason wake = board.wake_reason();
    const bool down_held = board.pressed(Button::Down);
    ESP_LOGI(kTag, "Wake reason=%d, down_held=%d, usb=%d, charging=%d",
             static_cast<int>(wake), down_held ? 1 : 0,
             board.usb_host_connected() ? 1 : 0, board.charging() ? 1 : 0);

    // Kick off initial background task immediately (main loop never freezes!)
    if (!web_state.config.has_wifi()) {
        enqueue_job(app, {NetJobKind::StartManage, false, true, "First-time setup"});
    } else if (wake == WakeReason::DownButton || down_held) {
        enqueue_job(app, {NetJobKind::StartManage, false, false, "Auto-sleep after 10 min on battery"});
    } else {
        enqueue_job(app, {NetJobKind::Sync, false, false, ""});
    }

    Buttons buttons;
    for (;;) {
        const auto now = now_ms();
        engine.display.tick(now);

        // LED: solid when e-paper refreshing/faulted, blinking during background sync, off when idle
        const bool syncing_now = app.worker_busy.load(std::memory_order_acquire);
        if (engine.display.busy() || engine.display.faulted()) {
            board.led(true);
        } else if (syncing_now) {
            board.led(((now / 250) % 2) == 0);
        } else {
            board.led(false);
        }

        // Always sample buttons every 20 ms (zero freeze!); ignore nav actions during active OTA write
        for (const auto b : {Button::Up, Button::Down, Button::Confirm}) {
            auto ev = buttons.sample(b, board.pressed(b), now);
            if (!ev || web.ota_in_progress()) continue;
            idle_since_ms = now;
            if (b == Button::Up && ev->press == Press::Long) {
                engine.display.retry(now);
            } else if (b == Button::Down && ev->press == Press::Long) {
                enqueue_job(app, {NetJobKind::StartManage, false, true, "Forced setup hotspot"});
            } else if (b == Button::Down && ev->press == Press::Short) {
                if (!app.in_manage_mode.load(std::memory_order_acquire)) {
                    enqueue_job(app, {NetJobKind::StartManage, false, false, "Auto-sleep after 10 min on battery"});
                }
            } else if (b == Button::Confirm && ev->press == Press::Short) {
                app.in_manage_mode.store(false, std::memory_order_release);
                enqueue_job(app, {NetJobKind::Sync, true, false, ""});
            }
        }

        // Render completed background Manage screen (non-blocking submit to DisplayScheduler)
        if (app.manage_screen_ready.exchange(false, std::memory_order_acq_rel)) {
            xSemaphoreTake(state_mutex, portMAX_DELAY);
            const bool is_ap = app.manage_ap;
            const std::string ssid = app.manage_ssid;
            const std::string url = app.manage_url;
            const std::string mac = web_state.mac_address;
            const std::string detail = app.manage_detail;
            xSemaphoreGive(state_mutex);

            engine.renderer.draw_manage(is_ap, ssid, url, mac, detail, engine.render_frame);
            engine.display.submit(engine.render_frame, now);
            idle_since_ms = now;
        }

        // Render completed background Sync result (non-blocking submit to DisplayScheduler)
        if (app.sync_result_ready.exchange(false, std::memory_order_acq_rel)) {
            xSemaphoreTake(state_mutex, portMAX_DELAY);
            const SyncReport rep = web_state.last_sync;
            const CalendarConfig cfg = web_state.config;
            const int bat_pct = web_state.battery_percent;
            const bool chg = web_state.charging;
            xSemaphoreGive(state_mutex);

            if (!rep.wifi_ok) {
                engine.renderer.draw_message(
                    "WIFI CONNECTION FAILED",
                    cfg.has_wifi() ? ("Could not join: " + cfg.wifi_ssid) : "No Wi-Fi configured yet.",
                    "Press Down button to open setup & management web UI.",
                    Color::Red, engine.render_frame);
                next_sleep_sec = 3600;
            } else if (!rep.time_ok) {
                engine.renderer.draw_message(
                    "TIME SYNC FAILED",
                    "Connected to Wi-Fi, but SNTP time sync failed.",
                    "Press Confirm to retry or Down to manage settings.",
                    Color::Red, engine.render_frame);
                next_sleep_sec = 3600;
            } else {
                CalendarViewStatus st;
                st.today = rep.today;
                st.sync_hour = rep.local_hour;
                st.sync_minute = rep.local_minute;
                st.wifi_connected = rep.wifi_ok;
                st.battery_percent = bat_pct;
                st.charging = chg;
                st.legend = cfg.calendars;
                if (rep.error_count > 0) {
                    char ebuf[24];
                    std::snprintf(ebuf, sizeof(ebuf), "%d ERR", rep.error_count);
                    st.error_text = ebuf;
                }
                engine.renderer.draw_agenda(st, rep.days, engine.render_frame);
                next_sleep_sec = CalendarConfig::seconds_until_next_sync(
                    rep.local_hour, rep.local_minute, rep.local_second,
                    cfg.sync_hour, cfg.sync_minute, cfg.sync_interval_minutes);
                synced_this_boot = true;
            }

            engine.display.submit(engine.render_frame, now);
            last_sync_ms = now;
            idle_since_ms = now;
            ESP_LOGI(kTag, "Background sync rendered; next scheduled sync in %lu s",
                     static_cast<unsigned long>(next_sleep_sec));
        }

        // Handle Web UI actions regardless of whether e-paper is on Manage or Calendar screen!
        bool force_sleep_now = false;
        if (web.running()) {
            if (web.take_reboot_request()) {
                ESP_LOGI(kTag, "OTA complete; rebooting into new firmware slot...");
                vTaskDelay(pdMS_TO_TICKS(900));
                web.stop();
                wifi.stop();
                esp_restart();
            } else if (web.take_wifi_changed()) {
                enqueue_job(app, {NetJobKind::ReconnectWifi, true, false, ""});
            } else if (web.take_sync_request()) {
                xSemaphoreTake(state_mutex, portMAX_DELAY);
                const bool is_ap = web_state.is_ap_mode;
                const bool has_w = web_state.config.has_wifi();
                xSemaphoreGive(state_mutex);
                if (is_ap && has_w) {
                    enqueue_job(app, {NetJobKind::ReconnectWifi, true, false, ""});
                } else if (!is_ap) {
                    app.in_manage_mode.store(false, std::memory_order_release);
                    enqueue_job(app, {NetJobKind::Sync, true, false, ""});
                }
            } else if (!web.ota_in_progress() && web.take_sleep_request()) {
                vTaskDelay(pdMS_TO_TICKS(300));
                app.in_manage_mode.store(false, std::memory_order_release);
                xSemaphoreTake(state_mutex, portMAX_DELAY);
                const bool has_w = web_state.config.has_wifi();
                xSemaphoreGive(state_mutex);
                if (!synced_this_boot && has_w) {
                    enqueue_job(app, {NetJobKind::Sync, false, false, ""});
                } else {
                    force_sleep_now = true;
                }
            }
        }

        if (app.in_manage_mode.load(std::memory_order_acquire)) {
            if (!web.ota_in_progress() &&
                !board.usb_host_connected() &&
                !syncing_now &&
                now - web.last_activity_ms() >= kManageTimeoutMs) {
                web.stop();
                app.in_manage_mode.store(false, std::memory_order_release);
                xSemaphoreTake(state_mutex, portMAX_DELAY);
                const bool has_w = web_state.config.has_wifi();
                xSemaphoreGive(state_mutex);
                if (!synced_this_boot && has_w) {
                    enqueue_job(app, {NetJobKind::Sync, false, false, ""});
                } else {
                    wifi.stop();
                    if (!has_w) {
                        engine.renderer.draw_message(
                            "SETUP PAUSED",
                            "Hotspot timed out to save battery.",
                            "Press Down button to restart NOTE4C-Setup.",
                            Color::Black, engine.render_frame);
                        engine.display.submit(engine.render_frame, now);
                    }
                    idle_since_ms = now;
                }
            }
        } else {
            // Outside Manage screen: sleep on battery after idle/web grace window, or auto-sync on USB timer
            const bool settled = !syncing_now &&
                                 !web.ota_in_progress() &&
                                 !has_pending_job(app) &&
                                 !engine.display.busy() &&
                                 !engine.display.pending() &&
                                 !buttons.held();
            if (settled) {
                const bool recent_web = web.running() &&
                                        web.had_http_activity() &&
                                        (now - web.last_activity_ms() < kManageTimeoutMs);
                const bool battery_idle_elapsed =
                    force_sleep_now ||
                    (!recent_web && (now - idle_since_ms >= kBatteryIdleBeforeSleepMs));
                if (!board.usb_host_connected() && battery_idle_elapsed) {
                    web.stop();
                    wifi.stop();
                    board.deep_sleep(next_sleep_sec);
                }
                if (board.usb_host_connected() &&
                    now - last_sync_ms >= static_cast<Millis>(next_sleep_sec) * 1000ULL) {
                    last_sync_ms = now;
                    enqueue_job(app, {NetJobKind::Sync, true, false, ""});
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
