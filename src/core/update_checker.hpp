/**
 * update_checker.hpp - Startup version check against GitHub Releases.
 *
 * On every launch, StarMiner spawns a background thread that polls the
 * GitHub Releases API. If a newer version is available the user sees a
 * one-line notice after the GPU detection banner. The check is
 * fire-and-forget: if the network is unreachable or the thread takes
 * longer than the caller's wait budget, the check is silently skipped.
 *
 * Requires STARMINER_HAVE_CURL (defined by CMakeLists when libcurl is
 * found). When curl is absent the exported functions are no-ops.
 */
#pragma once

#include <atomic>
#include <memory>
#include <string>

namespace starminer {

// GitHub coordinates — update only here if the repo moves.
static constexpr const char* kGitHubOwner     = "Soumya001";
static constexpr const char* kGitHubRepo      = "starminer";
static constexpr const char* kReleasesPageURL =
    "https://github.com/Soumya001/starminer/releases/latest";

/**
 * Compare semantic version strings ("1.2.3").
 * Returns true when `latest` is strictly newer than `current`.
 */
bool is_newer_version(const std::string& current, const std::string& latest);

/**
 * Blocking: hit the GitHub Releases API and return the tag_name of the
 * latest release (e.g. "1.0.1"), or "" on any error / network failure.
 * Timeout: 4 s connect + 6 s total.
 */
std::string fetch_latest_version();

/**
 * Handle for a background version check.
 * Owns two shared_ptrs so neither the thread nor the caller outlives the data.
 */
struct UpdateCheckHandle {
    std::shared_ptr<std::string>       result;  // written once before ready=true
    std::shared_ptr<std::atomic<bool>> ready;

    bool is_ready() const { return ready && ready->load(std::memory_order_acquire); }
    std::string get()  const { return result ? *result : ""; }
};

/**
 * Non-blocking: spawn a detached thread that calls fetch_latest_version()
 * and writes into the returned handle. The thread is truly detached — it
 * will not block program exit regardless of network latency.
 */
UpdateCheckHandle check_for_updates_async();

/**
 * Print the one-line update notice when `latest` is newer than the
 * compiled-in STARMINER_VERSION. No-op if up to date or latest is "".
 */
void print_update_notification(const std::string& latest);

/**
 * Handle `--update`: synchronously fetch latest version, print status,
 * and show download/upgrade instructions. Returns a suggested exit code
 * (0 = up to date or instructions shown, 1 = fetch failed with no curl).
 */
int run_update_command();

}  // namespace starminer
