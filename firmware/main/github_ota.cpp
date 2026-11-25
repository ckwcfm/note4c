#include "github_ota.hpp"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

namespace note4c::idf {
namespace {
constexpr const char* kTag = "gh_ota";

bool valid_repo_slug(std::string_view repo) {
    if (repo.size() < 3 || repo.size() > 100) return false;
    size_t slashes = 0;
    for (char c : repo) {
        if (c == '/') {
            ++slashes;
        } else if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') {
            return false;
        }
    }
    return slashes == 1 && repo.front() != '/' && repo.back() != '/';
}

// Extract a JSON string value for key starting at search_pos (minimal, zero-dependency).
bool find_json_string(std::string_view json, std::string_view key, size_t start_pos,
                      std::string& out, size_t* next_pos = nullptr) {
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t pos = json.find(needle, start_pos);
    while (pos != std::string_view::npos) {
        size_t p = pos + needle.size();
        while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) ++p;
        if (p < json.size() && json[p] == ':') {
            ++p;
            while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) ++p;
            if (p < json.size() && json[p] == '"') {
                ++p;
                std::string val;
                while (p < json.size()) {
                    const char c = json[p++];
                    if (c == '"') {
                        out = std::move(val);
                        if (next_pos) *next_pos = p;
                        return true;
                    }
                    if (c == '\\' && p < json.size()) {
                        const char e = json[p++];
                        if (e == '"' || e == '\\' || e == '/') val.push_back(e);
                        else if (e == 'n') val.push_back('\n');
                    } else {
                        val.push_back(c);
                    }
                }
                return false;
            }
        }
        pos = json.find(needle, pos + needle.size());
    }
    return false;
}

bool find_json_uint(std::string_view json, std::string_view key, size_t start_pos, size_t& out) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const size_t pos = json.find(needle, start_pos);
    if (pos == std::string_view::npos) return false;
    size_t p = pos + needle.size();
    while (p < json.size() && (std::isspace(static_cast<unsigned char>(json[p])) || json[p] == ':')) ++p;
    if (p >= json.size() || !std::isdigit(static_cast<unsigned char>(json[p]))) return false;
    size_t v = 0;
    while (p < json.size() && std::isdigit(static_cast<unsigned char>(json[p]))) {
        v = v * 10 + static_cast<size_t>(json[p++] - '0');
    }
    out = v;
    return true;
}

struct JsonBodyCtx {
    std::string body;
};

esp_err_t json_http_cb(esp_http_client_event_t* evt) {
    auto* ctx = static_cast<JsonBodyCtx*>(evt->user_data);
    if (!ctx) return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        if (esp_http_client_get_status_code(evt->client) == 200 && ctx->body.size() < 32768) {
            const size_t space = 32768 - ctx->body.size();
            const size_t take = std::min<size_t>(space, static_cast<size_t>(evt->data_len));
            ctx->body.append(static_cast<const char*>(evt->data), take);
        }
    }
    return ESP_OK;
}

struct OtaStreamCtx {
    const esp_partition_t* update = nullptr;
    OtaProgress* progress = nullptr;
    esp_ota_handle_t handle = 0;
    bool started = false;
    bool header_checked = false;
    bool failed = false;
    std::string error;
    std::vector<uint8_t> header_buf;
};

esp_err_t ota_http_cb(esp_http_client_event_t* evt) {
    auto* ctx = static_cast<OtaStreamCtx*>(evt->user_data);
    if (!ctx || ctx->failed) return ESP_OK;

    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value) {
        if (strcasecmp(evt->header_key, "Content-Length") == 0) {
            const long long cl = std::atoll(evt->header_value);
            if (cl > 0 && esp_http_client_get_status_code(evt->client) == 200) {
                ctx->progress->total.store(static_cast<size_t>(cl), std::memory_order_release);
            }
        }
        return ESP_OK;
    }

    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }
    if (esp_http_client_get_status_code(evt->client) != 200) {
        return ESP_OK;
    }

    const auto* chunk = static_cast<const uint8_t*>(evt->data);
    const size_t len = static_cast<size_t>(evt->data_len);
    constexpr size_t kMinHeader = 32 + sizeof(esp_app_desc_t);

    if (!ctx->header_checked) {
        ctx->header_buf.insert(ctx->header_buf.end(), chunk, chunk + len);
        if (ctx->header_buf.size() < kMinHeader) return ESP_OK;

        const uint8_t magic = ctx->header_buf[0];
        const uint16_t chip_id = static_cast<uint16_t>(ctx->header_buf[12]) |
                                 static_cast<uint16_t>(static_cast<uint16_t>(ctx->header_buf[13]) << 8);
        esp_app_desc_t desc{};
        std::memcpy(&desc, ctx->header_buf.data() + 32, sizeof(desc));
        if (magic != 0xE9 || chip_id != 9 || desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
            ctx->failed = true;
            ctx->error = "Downloaded file is not a valid ESP32-S3 firmware image";
            return ESP_FAIL;
        }
        if (std::strncmp(desc.project_name, "note4c_framework", sizeof(desc.project_name)) != 0) {
            ctx->failed = true;
            ctx->error = "Rejected: firmware is not note4c_framework";
            return ESP_FAIL;
        }
        const esp_err_t berr = esp_ota_begin(ctx->update, OTA_WITH_SEQUENTIAL_WRITES, &ctx->handle);
        if (berr != ESP_OK) {
            ctx->failed = true;
            ctx->error = std::string("esp_ota_begin failed: ") + esp_err_to_name(berr);
            return ESP_FAIL;
        }
        ctx->started = true;
        ctx->header_checked = true;

        const esp_err_t werr = esp_ota_write(ctx->handle, ctx->header_buf.data(), ctx->header_buf.size());
        if (werr != ESP_OK) {
            ctx->failed = true;
            ctx->error = std::string("esp_ota_write failed: ") + esp_err_to_name(werr);
            return ESP_FAIL;
        }
        ctx->progress->written.fetch_add(ctx->header_buf.size(), std::memory_order_acq_rel);
        ctx->header_buf.clear();
        return ESP_OK;
    }

    const size_t next_total = ctx->progress->written.load(std::memory_order_acquire) + len;
    if (next_total > ctx->update->size) {
        ctx->failed = true;
        ctx->error = "Firmware exceeds OTA partition size";
        return ESP_FAIL;
    }
    const esp_err_t werr = esp_ota_write(ctx->handle, chunk, len);
    if (werr != ESP_OK) {
        ctx->failed = true;
        ctx->error = std::string("esp_ota_write failed: ") + esp_err_to_name(werr);
        return ESP_FAIL;
    }
    ctx->progress->written.fetch_add(len, std::memory_order_acq_rel);
    return ESP_OK;
}

} // namespace

int compare_semver(std::string_view a, std::string_view b) {
    auto parse = [](std::string_view s) -> std::array<int, 3> {
        std::array<int, 3> v{0, 0, 0};
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.remove_prefix(1);
        size_t pos = 0;
        for (int idx = 0; idx < 3 && pos < s.size(); ++idx) {
            int num = 0;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
                num = num * 10 + (s[pos++] - '0');
            }
            v[static_cast<size_t>(idx)] = num;
            if (pos < s.size() && s[pos] == '.') ++pos;
            else break;
        }
        return v;
    };
    const auto pa = parse(a);
    const auto pb = parse(b);
    for (size_t i = 0; i < 3; ++i) {
        if (pa[i] != pb[i]) return pa[i] > pb[i] ? 1 : -1;
    }
    return 0;
}

GitHubReleaseInfo check_github_release(std::string_view repo) {
    GitHubReleaseInfo info;
    if (!valid_repo_slug(repo)) {
        info.error = "Invalid GitHub repo format (expected owner/repo)";
        return info;
    }
    const std::string api_url = "https://api.github.com/repos/" + std::string(repo) + "/releases/latest";
    JsonBodyCtx ctx;

    esp_http_client_config_t hcfg{};
    hcfg.url = api_url.c_str();
    hcfg.method = HTTP_METHOD_GET;
    hcfg.timeout_ms = 20000;
    hcfg.buffer_size = 4096;
    hcfg.buffer_size_tx = 2048;
    hcfg.max_redirection_count = 3;
    hcfg.event_handler = json_http_cb;
    hcfg.user_data = &ctx;
    hcfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    if (!client) {
        info.error = "HTTP client init failed";
        return info;
    }
    esp_http_client_set_header(client, "User-Agent", "NOTE4C-Calendar/1.0");
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");

    const esp_err_t err = esp_http_client_perform(client);
    const int code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        info.error = std::string("GitHub request failed: ") + esp_err_to_name(err);
        return info;
    }
    if (code == 404) {
        info.error = "No published GitHub Release found yet (push a v* tag first)";
        return info;
    }
    if (code != 200) {
        info.error = "GitHub API HTTP " + std::to_string(code);
        return info;
    }

    if (!find_json_string(ctx.body, "tag_name", 0, info.tag)) {
        info.error = "Could not parse tag_name from GitHub release";
        return info;
    }

    // Locate the note4c_framework.bin asset entry
    const size_t assets_pos = ctx.body.find("\"assets\"");
    const size_t bin_pos = ctx.body.find("\"note4c_framework.bin\"",
                                         assets_pos == std::string::npos ? 0 : assets_pos);
    if (bin_pos == std::string::npos) {
        info.error = "Release " + info.tag + " has no note4c_framework.bin asset yet";
        return info;
    }
    if (!find_json_string(ctx.body, "browser_download_url", bin_pos, info.asset_url)) {
        info.error = "Could not find download URL for note4c_framework.bin";
        return info;
    }
    find_json_uint(ctx.body, "size", bin_pos, info.asset_size);
    info.ok = true;
    ESP_LOGI(kTag, "Found GitHub release %s (%u bytes)",
             info.tag.c_str(), static_cast<unsigned>(info.asset_size));
    return info;
}

bool install_github_ota(std::string_view asset_url,
                        OtaProgress& progress,
                        std::string& out_slot,
                        std::string& out_error) {
    if (asset_url.rfind("https://", 0) != 0) {
        out_error = "OTA URL must start with https://";
        return false;
    }
    const esp_partition_t* update = esp_ota_get_next_update_partition(nullptr);
    if (!update) {
        out_error = "No inactive OTA partition available";
        return false;
    }

    progress.written.store(0, std::memory_order_release);
    progress.total.store(0, std::memory_order_release);
    progress.active.store(true, std::memory_order_release);

    OtaStreamCtx ctx;
    ctx.update = update;
    ctx.progress = &progress;
    ctx.header_buf.reserve(512);

    std::string url_str(asset_url);
    esp_http_client_config_t hcfg{};
    hcfg.url = url_str.c_str();
    hcfg.method = HTTP_METHOD_GET;
    hcfg.timeout_ms = 30000;
    hcfg.buffer_size = 4096;
    hcfg.buffer_size_tx = 2048; // Handles long presigned GitHub/S3 redirect URLs
    hcfg.max_redirection_count = 5;
    hcfg.event_handler = ota_http_cb;
    hcfg.user_data = &ctx;
    hcfg.crt_bundle_attach = esp_crt_bundle_attach;

    ESP_LOGI(kTag, "Downloading GitHub OTA into '%s' @ 0x%08lx...",
             update->label, static_cast<unsigned long>(update->address));

    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    if (!client) {
        progress.active.store(false, std::memory_order_release);
        out_error = "HTTP client init failed";
        return false;
    }
    esp_http_client_set_header(client, "User-Agent", "NOTE4C-Calendar/1.0");
    esp_http_client_set_header(client, "Accept", "application/octet-stream");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");

    const esp_err_t err = esp_http_client_perform(client);
    const int code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    auto abort_and_fail = [&](const std::string& msg) {
        if (ctx.started) esp_ota_abort(ctx.handle);
        progress.active.store(false, std::memory_order_release);
        out_error = msg;
        ESP_LOGE(kTag, "GitHub OTA failed: %s", msg.c_str());
        return false;
    };

    if (ctx.failed) return abort_and_fail(ctx.error);
    if (err != ESP_OK) return abort_and_fail(std::string("Download failed: ") + esp_err_to_name(err));
    if (code != 200) return abort_and_fail("Download HTTP " + std::to_string(code));
    if (!ctx.started) return abort_and_fail("Downloaded file too small to be valid firmware");

    ctx.started = false;
    const esp_err_t eerr = esp_ota_end(ctx.handle);
    if (eerr != ESP_OK) {
        return abort_and_fail(std::string("Image verification failed: ") + esp_err_to_name(eerr));
    }
    const esp_err_t serr = esp_ota_set_boot_partition(update);
    if (serr != ESP_OK) {
        return abort_and_fail(std::string("Failed to set boot slot: ") + esp_err_to_name(serr));
    }

    progress.active.store(false, std::memory_order_release);
    out_slot = update->label;
    ESP_LOGI(kTag, "GitHub OTA complete (%u bytes) -> next boot slot '%s'",
             static_cast<unsigned>(progress.written.load(std::memory_order_acquire)),
             out_slot.c_str());
    return true;
}

} // namespace note4c::idf
