#pragma once
#include <atomic>
#include <cstddef>
#include <string>
#include <string_view>

namespace note4c::idf {

inline constexpr const char* kDefaultGitHubRepo = "ckwcfm/note4c";

struct GitHubReleaseInfo {
    bool ok = false;
    std::string tag;
    std::string asset_url;
    size_t asset_size = 0;
    std::string error;
};

struct OtaProgress {
    std::atomic<bool> active{false};
    std::atomic<size_t> written{0};
    std::atomic<size_t> total{0};
};

// Queries GitHub Releases API for the latest release containing note4c_framework.bin.
GitHubReleaseInfo check_github_release(std::string_view repo);

// Streams a verified NOTE4C firmware binary over HTTPS into the inactive OTA slot.
// Leaves the current firmware untouched if download, header check, or hash verification fails.
bool install_github_ota(std::string_view asset_url,
                        OtaProgress& progress,
                        std::string& out_slot,
                        std::string& out_error);

} // namespace note4c::idf
