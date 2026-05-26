/**
 * update_checker.cpp - GitHub Releases version check implementation.
 */
#include "update_checker.hpp"

#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "edition.hpp"  // STARMINER_VERSION

#ifdef STARMINER_HAVE_CURL
#include <curl/curl.h>
#endif

namespace starminer {

namespace {

// Split "1.2.3" → {1, 2, 3}.  Non-numeric components become 0.
std::vector<int> parse_semver(const std::string& v) {
    std::vector<int> parts;
    std::string s = v;
    // Strip a leading 'v'
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s = s.substr(1);
    std::istringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, '.')) {
        try { parts.push_back(std::stoi(tok)); } catch (...) { parts.push_back(0); }
    }
    while (parts.size() < 3) parts.push_back(0);
    return parts;
}

#ifdef STARMINER_HAVE_CURL
// libcurl write callback — appends received bytes to a std::string.
std::size_t curl_write_cb(void* ptr, std::size_t size,
                           std::size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(static_cast<const char*>(ptr), size * nmemb);
    return size * nmemb;
}

// Extract a JSON string field value: "tag_name": "v1.0.1" → "v1.0.1".
// No JSON library — the GitHub response is small and well-formed.
std::string extract_string_field(const std::string& body,
                                  const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    // Skip whitespace and colon
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == ':')) ++pos;
    if (pos >= body.size() || body[pos] != '"') return "";
    ++pos;  // skip opening quote
    auto end = body.find('"', pos);
    if (end == std::string::npos) return "";
    return body.substr(pos, end - pos);
}
#endif  // STARMINER_HAVE_CURL

}  // namespace

// ---------------------------------------------------------------------------

bool is_newer_version(const std::string& current, const std::string& latest) {
    if (latest.empty()) return false;
    const auto cur = parse_semver(current);
    const auto lat = parse_semver(latest);
    for (std::size_t i = 0; i < 3; ++i) {
        if (lat[i] > cur[i]) return true;
        if (lat[i] < cur[i]) return false;
    }
    return false;  // equal
}

std::string fetch_latest_version() {
#ifndef STARMINER_HAVE_CURL
    return "";
#else
    // GitHub API: GET /repos/:owner/:repo/releases/latest
    std::string url = std::string("https://api.github.com/repos/") +
                      kGitHubOwner + "/" + kGitHubRepo + "/releases/latest";

    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string body;
    body.reserve(4096);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 6L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);
    // GitHub API requires a User-Agent header.
    std::string ua = std::string("starminer/") + STARMINER_VERSION;
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua.c_str());
    // Accept JSON
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    const CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK || http_code != 200) return "";

    // Extract "tag_name" from the JSON response.
    std::string tag = extract_string_field(body, "tag_name");
    // Strip leading 'v' so "v1.0.1" → "1.0.1" for comparison.
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag = tag.substr(1);
    return tag;
#endif
}

UpdateCheckHandle check_for_updates_async() {
    UpdateCheckHandle h;
    h.result = std::make_shared<std::string>();
    h.ready  = std::make_shared<std::atomic<bool>>(false);

    // Detached thread: truly fire-and-forget. The shared_ptrs keep the data
    // alive even if main() returns before the thread completes.
    std::thread([result = h.result, ready = h.ready]() {
        *result = fetch_latest_version();
        ready->store(true, std::memory_order_release);
    }).detach();

    return h;
}

void print_update_notification(const std::string& latest) {
    if (!is_newer_version(STARMINER_VERSION, latest)) return;

    // Use ANSI yellow for visibility — same convention as Interactive::warning_message.
    std::cout << "\033[33m[!]\033[0m New version \033[1mv" << latest
              << "\033[0m available (current: v" << STARMINER_VERSION << ")\n"
              << "    Download: " << kReleasesPageURL << "\n"
              << "    Or run:   starminer --update\n\n";
}

int run_update_command() {
#ifndef STARMINER_HAVE_CURL
    std::cout << "[*] Current version: v" << STARMINER_VERSION << "\n";
    std::cout << "[!] libcurl is not available — cannot check for updates automatically.\n";
    std::cout << "    Download the latest release manually:\n";
    std::cout << "    " << kReleasesPageURL << "\n";
    return 1;
#else
    std::cout << "[*] Current version: v" << STARMINER_VERSION << "\n";
    std::cout << "[*] Checking for updates...\n" << std::flush;

    const std::string latest = fetch_latest_version();
    if (latest.empty()) {
        std::cout << "[!] Could not reach GitHub. Check your network connection.\n";
        std::cout << "    Releases page: " << kReleasesPageURL << "\n";
        return 1;
    }

    if (is_newer_version(STARMINER_VERSION, latest)) {
        std::cout << "\033[32m[+]\033[0m New version \033[1mv" << latest
                  << "\033[0m is available!\n\n";
        std::cout << "    Download: " << kReleasesPageURL << "\n\n";

        // Platform-specific download hint
#if defined(_WIN32)
        std::cout << "    Windows: download the .zip from the releases page above.\n";
#elif defined(__APPLE__)
        std::cout << "    macOS:  curl -L " << kReleasesPageURL
                  << " -o starminer && chmod +x starminer\n";
#else
        std::cout << "    Linux:  wget " << kReleasesPageURL
                  << " -O starminer && chmod +x starminer\n";
        std::cout << "            Or: curl -L " << kReleasesPageURL
                  << " -o starminer && chmod +x starminer\n";
#endif
    } else {
        std::cout << "\033[32m[+]\033[0m You are running the latest version (v"
                  << STARMINER_VERSION << ").\n";
    }
    return 0;
#endif
}

}  // namespace starminer
