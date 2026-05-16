// http_pool_client.cpp - HTTP/JSON pool client implementation

#include "http_pool_client.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <chrono>

#ifdef COLLIDER_HAVE_CURL
#include <curl/curl.h>
#endif

namespace collider {
namespace pool {

// ---------------------------------------------------------------------------
// libcurl write callback
// ---------------------------------------------------------------------------
#ifdef COLLIDER_HAVE_CURL
static size_t curl_write_callback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}
#endif

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
HttpPoolClient::HttpPoolClient()
    : timeout_ms_(30000)
    , use_tls_(true)
    , verify_cert_(true)
    , batch_size_(DEFAULT_BATCH_SIZE)
    , connected_(false)
    , running_(false)
    , dp_sequence_(0)
    , flush_stop_(false)
{
}

HttpPoolClient::~HttpPoolClient() {
    disconnect();
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------
bool HttpPoolClient::connect(const std::string& host, uint16_t port) {
#ifndef COLLIDER_HAVE_CURL
    std::cerr << "[HttpPoolClient] libcurl not available at build time. "
              << "HTTP pool support requires libcurl."
              << std::endl;
    return false;
#endif

    std::string scheme = use_tls_ ? "https" : "http";
    base_url_ = scheme + "://" + host;
    if ((use_tls_ && port != 443) || (!use_tls_ && port != 80)) {
        base_url_ += ":" + std::to_string(port);
    }

    connected_.store(true);
    running_.store(true);
    flush_stop_.store(false);

    // Start background flush thread
    if (flush_thread_.joinable()) {
        flush_stop_.store(true);
        dp_cv_.notify_all();
        flush_thread_.join();
        flush_stop_.store(false);
    }
    flush_thread_ = std::thread([this]() { this->flush_loop(); });

    std::cout << "[HttpPoolClient] Base URL: " << base_url_ << std::endl;
    return true;
}

void HttpPoolClient::disconnect() {
    flush_stop_.store(true);
    dp_cv_.notify_all();
    if (flush_thread_.joinable()) {
        flush_thread_.join();
    }

    // Flush any remaining DPs
    if (!dp_queue_.empty()) {
        flush_batch();
    }

    connected_.store(false);
    running_.store(false);
    session_token_.clear();
}

bool HttpPoolClient::is_connected() const {
    return connected_.load() && running_.load();
}

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------
bool HttpPoolClient::authenticate(const std::string& worker_name,
                                 const std::string& password) {
    if (!is_connected()) return false;

    worker_name_ = worker_name;
    password_ = password;

    std::ostringstream json;
    json << "{";
    json << "\"worker_name\":\"" << worker_name << "\",";
    json << "\"version\":\"1.5.0-star\",";
    json << "\"pool_type\":\"http\"";
    if (!password.empty()) {
        json << ",\"password\":\"" << password << "\"";
    }
    json << "}";

    std::string response;
    if (!http_post("/api/v1/pool/auth", json.str(), response)) {
        std::cerr << "[HttpPoolClient] Authentication request failed" << std::endl;
        return false;
    }

    // Simple JSON parsing for session_token
    auto pos = response.find("\"session_token\"");
    if (pos == std::string::npos) {
        pos = response.find("\"auth_ok\"");
        if (pos != std::string::npos && response.find("true", pos) != std::string::npos) {
            // Auth OK but no token - server may not use tokens
            std::cout << "[HttpPoolClient] Authenticated (no token required)" << std::endl;
            return true;
        }
        std::cerr << "[HttpPoolClient] Authentication rejected: " << response << std::endl;
        return false;
    }

    auto start = response.find('"', pos + 17);
    if (start != std::string::npos) {
        auto end = response.find('"', start + 1);
        if (end != std::string::npos) {
            session_token_ = response.substr(start + 1, end - start - 1);
        }
    }

    std::cout << "[HttpPoolClient] Authenticated as " << worker_name << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// Work management
// ---------------------------------------------------------------------------
bool HttpPoolClient::request_work(WorkAssignment& work) {
    if (!is_connected()) return false;

    std::ostringstream json;
    json << "{";
    json << "\"worker_name\":\"" << worker_name_ << "\"";
    if (!session_token_.empty()) {
        json << ",\"session_token\":\"" << session_token_ << "\"";
    }
    json << "}";

    std::string response;
    if (!http_post("/api/v1/pool/work", json.str(), response)) {
        return false;
    }

    // Parse JSON response manually (simple key-value extraction)
    auto extract_string = [&](const std::string& key) -> std::string {
        auto pos = response.find("\"" + key + "\"");
        if (pos == std::string::npos) return "";
        auto colon = response.find(':', pos);
        if (colon == std::string::npos) return "";
        size_t i = colon + 1;
        while (i < response.size() && (response[i] == ' ' || response[i] == '\t' || response[i] == '\n' || response[i] == '\r')) i++;
        if (i >= response.size()) return "";
        if (response[i] == '"') {
            // Quoted string
            auto end = response.find('"', i + 1);
            if (end == std::string::npos) return "";
            return response.substr(i + 1, end - i - 1);
        } else {
            // Number or boolean (unquoted)
            auto end = i;
            while (end < response.size() && response[end] != ',' && response[end] != '}') end++;
            // Trim trailing whitespace
            while (end > i && (response[end - 1] == ' ' || response[end - 1] == '\t' || response[end - 1] == '\n' || response[end - 1] == '\r')) end--;
            return response.substr(i, end - i);
        }
    };

    std::string pub_key_hex = extract_string("public_key");
    std::string range_start_hex = extract_string("range_start");
    std::string range_end_hex = extract_string("range_end");
    std::string dp_bits_str = extract_string("dp_bits");
    std::string work_id_str = extract_string("work_id");

    if (pub_key_hex.empty() || range_start_hex.empty() || range_end_hex.empty()) {
        std::cerr << "[HttpPoolClient] Invalid work assignment response" << std::endl;
        return false;
    }

    // Decode hex to bytes
    hex_to_bytes(pub_key_hex, work.public_key, 33);
    hex_to_bytes(range_start_hex, work.range_start, 32);
    hex_to_bytes(range_end_hex, work.range_end, 32);

    work.dp_bits = dp_bits_str.empty() ? 28 : static_cast<uint32_t>(std::stoul(dp_bits_str));
    work.work_id = work_id_str.empty() ? 0 : static_cast<uint64_t>(std::stoull(work_id_str));
    work.puzzle_name = "Puzzle #135";
    current_work_id_.store(work.work_id, std::memory_order_release);

    // Reset DP sequence for new work
    dp_sequence_.store(0, std::memory_order_release);

    // Notify work callback
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (work_callback_) {
            work_callback_(work);
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// DP submission (queued, flushed asynchronously)
// ---------------------------------------------------------------------------
bool HttpPoolClient::submit_dp(const DistinguishedPoint& dp) {
    std::lock_guard<std::mutex> lock(dp_mutex_);
    if (dp_queue_.size() >= MAX_DP_QUEUE_SIZE) {
        return false; // Backpressure
    }
    dp_queue_.push(dp);
    dp_cv_.notify_one();
    return true;
}

bool HttpPoolClient::submit_dps(const std::vector<DistinguishedPoint>& dps) {
    std::lock_guard<std::mutex> lock(dp_mutex_);
    if (dp_queue_.size() + dps.size() > MAX_DP_QUEUE_SIZE) {
        return false;
    }
    for (const auto& dp : dps) {
        dp_queue_.push(dp);
    }
    dp_cv_.notify_one();
    return true;
}

// ---------------------------------------------------------------------------
// Background flush loop
// ---------------------------------------------------------------------------
void HttpPoolClient::flush_loop() {
    while (!flush_stop_.load()) {
        std::unique_lock<std::mutex> lock(dp_mutex_);
        // Wait until: stop signal, queue full, or timeout
        dp_cv_.wait_for(lock, std::chrono::milliseconds(FLUSH_INTERVAL_MS),
            [this]() {
                return flush_stop_.load() || dp_queue_.size() >= batch_size_;
            });

        size_t queue_size = dp_queue_.size();
        lock.unlock();

        if (queue_size > 0) {
            flush_batch();
        }
    }
}

bool HttpPoolClient::flush_batch() {
    std::vector<DistinguishedPoint> batch;
    {
        std::lock_guard<std::mutex> lock(dp_mutex_);
        while (!dp_queue_.empty() && batch.size() < batch_size_) {
            batch.push_back(dp_queue_.front());
            dp_queue_.pop();
        }
    }

    if (batch.empty()) return true;

    // Build JSON payload
    std::ostringstream json;
    json << "{";
    json << "\"worker_name\":\"" << worker_name_ << "\",";
    if (!session_token_.empty()) {
        json << "\"session_token\":\"" << session_token_ << "\",";
    }
    json << "\"work_id\":" << current_work_id_.load(std::memory_order_relaxed) << ",";
    json << "\"dps\":[";
    for (size_t i = 0; i < batch.size(); ++i) {
        const auto& dp = batch[i];
        uint32_t seq = dp_sequence_.fetch_add(1, std::memory_order_relaxed);
        json << "{";
        json << "\"x\":\"" << bytes_to_hex(dp.x, 32) << "\",";
        json << "\"d\":\"" << bytes_to_hex(dp.d, 32) << "\",";
        json << "\"type\":" << static_cast<int>(dp.type) << ",";
        json << "\"sequence\":" << seq << ",";
        json << "\"dp_bits\":" << dp.dp_bits;
        json << "}";
        if (i + 1 < batch.size()) json << ",";
    }
    json << "]}";

    std::string response;
    if (!http_post("/api/v1/pool/dp_batch", json.str(), response)) {
        // Re-queue failed DPs? For now, drop them to avoid infinite retry
        std::cerr << "[HttpPoolClient] Failed to submit batch of " << batch.size()
                  << " DPs" << std::endl;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------
PoolStats HttpPoolClient::get_stats() {
    if (!is_connected()) return PoolStats{};

    // Cache stats for 2 seconds to avoid hammering the server
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(
                now - last_stats_fetch_).count() < 2) {
            return cached_stats_;
        }
    }

    std::string endpoint = "/api/v1/pool/stats?worker=" + worker_name_;
    if (!session_token_.empty()) {
        endpoint += "&token=" + session_token_;
    }

    std::string response;
    if (!http_get(endpoint, response)) {
        return cached_stats_;
    }

    // Parse JSON response
    auto extract_u64 = [&](const std::string& key) -> uint64_t {
        auto pos = response.find("\"" + key + "\"");
        if (pos == std::string::npos) return 0;
        auto colon = response.find(':', pos);
        if (colon == std::string::npos) return 0;
        size_t start = colon + 1;
        while (start < response.size() && (response[start] == ' ' || response[start] == '"')) start++;
        size_t end = start;
        while (end < response.size() && response[end] != ',' && response[end] != '}' && response[end] != '"') end++;
        try {
            return std::stoull(response.substr(start, end - start));
        } catch (...) {
            return 0;
        }
    };

    auto extract_u32 = [&](const std::string& key) -> uint32_t {
        auto pos = response.find("\"" + key + "\"");
        if (pos == std::string::npos) return 0;
        auto colon = response.find(':', pos);
        if (colon == std::string::npos) return 0;
        size_t start = colon + 1;
        while (start < response.size() && (response[start] == ' ' || response[start] == '"')) start++;
        size_t end = start;
        while (end < response.size() && response[end] != ',' && response[end] != '}' && response[end] != '"') end++;
        try {
            return static_cast<uint32_t>(std::stoul(response.substr(start, end - start)));
        } catch (...) {
            return 0;
        }
    };

    auto extract_float = [&](const std::string& key) -> float {
        auto pos = response.find("\"" + key + "\"");
        if (pos == std::string::npos) return 0.0f;
        auto colon = response.find(':', pos);
        if (colon == std::string::npos) return 0.0f;
        size_t start = colon + 1;
        while (start < response.size() && (response[start] == ' ' || response[start] == '"')) start++;
        size_t end = start;
        while (end < response.size() && response[end] != ',' && response[end] != '}' && response[end] != '"') end++;
        try {
            return std::stof(response.substr(start, end - start));
        } catch (...) {
            return 0.0f;
        }
    };

    PoolStats stats;
    stats.total_dps = extract_u64("total_dps");
    stats.total_workers = static_cast<uint32_t>(extract_u64("total_workers"));
    stats.active_workers = static_cast<uint32_t>(extract_u64("active_workers"));
    stats.dps_per_second = extract_float("dps_per_second");
    stats.your_share = extract_float("your_share");
    stats.your_dps = extract_u64("your_dps");
    stats.uptime_seconds = extract_u32("uptime_seconds");
    stats.connected_workers = stats.active_workers;

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        cached_stats_ = stats;
        last_stats_fetch_ = std::chrono::steady_clock::now();
    }

    return stats;
}

// ---------------------------------------------------------------------------
// Solution reporting
// ---------------------------------------------------------------------------
bool HttpPoolClient::report_solution(const uint8_t* private_key) {
    if (!is_connected()) return false;

    std::ostringstream json;
    json << "{";
    json << "\"worker_name\":\"" << worker_name_ << "\",";
    if (!session_token_.empty()) {
        json << "\"session_token\":\"" << session_token_ << "\",";
    }
    json << "\"private_key\":\"" << bytes_to_hex(private_key, 32) << "\"";
    json << "}";

    std::string response;
    bool ok = http_post("/api/v1/pool/solution", json.str(), response);

    // Notify callback
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (solution_callback_) {
            solution_callback_(private_key);
        }
    }

    return ok;
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------
void HttpPoolClient::set_solution_callback(SolutionCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    solution_callback_ = cb;
}

void HttpPoolClient::set_work_callback(WorkCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    work_callback_ = cb;
}

// ---------------------------------------------------------------------------
// HTTP helpers (libcurl)
// ---------------------------------------------------------------------------
#ifdef COLLIDER_HAVE_CURL
bool HttpPoolClient::http_post(const std::string& endpoint,
                               const std::string& json_payload,
                               std::string& response_out) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "[HttpPoolClient] curl_easy_init failed" << std::endl;
        return false;
    }

    std::string url = base_url_ + endpoint;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms_));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, verify_cert_ ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, verify_cert_ ? 2L : 0L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::cerr << "[HttpPoolClient] HTTP POST failed: " << curl_easy_strerror(res)
                  << " (URL: " << url << ")" << std::endl;
        return false;
    }

    if (http_code < 200 || http_code >= 300) {
        std::cerr << "[HttpPoolClient] HTTP " << http_code << ": " << response_out << std::endl;
        return false;
    }

    return true;
}

bool HttpPoolClient::http_get(const std::string& endpoint,
                              std::string& response_out) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string url = base_url_ + endpoint;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms_));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, verify_cert_ ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, verify_cert_ ? 2L : 0L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::cerr << "[HttpPoolClient] HTTP GET failed: " << curl_easy_strerror(res)
                  << " (URL: " << url << ")" << std::endl;
        return false;
    }

    if (http_code < 200 || http_code >= 300) {
        return false;
    }

    return true;
}
#else
bool HttpPoolClient::http_post(const std::string&, const std::string&, std::string&) {
    std::cerr << "[HttpPoolClient] libcurl not available" << std::endl;
    return false;
}
bool HttpPoolClient::http_get(const std::string&, std::string&) {
    std::cerr << "[HttpPoolClient] libcurl not available" << std::endl;
    return false;
}
#endif

// ---------------------------------------------------------------------------
// Hex encoding / decoding
// ---------------------------------------------------------------------------
std::string HttpPoolClient::bytes_to_hex(const uint8_t* data, size_t len) const {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        oss << std::setw(2) << static_cast<int>(data[i]);
    }
    return oss.str();
}

bool HttpPoolClient::hex_to_bytes(const std::string& hex, uint8_t* out, size_t out_len) const {
    std::string clean = hex;
    if (clean.size() >= 2 && clean[0] == '0' && (clean[1] == 'x' || clean[1] == 'X')) {
        clean = clean.substr(2);
    }
    if (clean.size() != out_len * 2) {
        return false;
    }
    for (size_t i = 0; i < out_len; ++i) {
        std::string byte_str = clean.substr(i * 2, 2);
        try {
            out[i] = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
        } catch (...) {
            return false;
        }
    }
    return true;
}

} // namespace pool
} // namespace collider
