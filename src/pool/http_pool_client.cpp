// http_pool_client.cpp - HTTP/JSON pool client implementation for StarMiner

#include "http_pool_client.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <chrono>
#include <random>

#include "../../third_party/nlohmann/json.hpp"
using json = nlohmann::json;

#ifdef COLLIDER_HAVE_CURL
#include <curl/curl.h>
#endif

namespace collider {
namespace pool {

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
    , current_work_id_(0)
    , threads_stop_(false)
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
    std::cerr << "[StarMiner] libcurl not available at build time. "
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
    threads_stop_.store(false);

    if (flush_thread_.joinable()) flush_thread_.join();
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();

    flush_thread_ = std::thread([this]() { this->flush_loop(); });
    heartbeat_thread_ = std::thread([this]() { this->heartbeat_loop(); });

    std::cout << "[StarMiner] Pool URL: " << base_url_ << std::endl;
    return true;
}

void HttpPoolClient::disconnect() {
    threads_stop_.store(true);
    dp_cv_.notify_all();

    if (flush_thread_.joinable()) flush_thread_.join();
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();

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
// Telemetry
// ---------------------------------------------------------------------------
void HttpPoolClient::set_gpu_telemetry(const std::map<int, std::string>& gpu_names,
                                       const std::map<int, double>& gpu_mhs) {
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    gpu_names_ = gpu_names;
    gpu_mhs_ = gpu_mhs;
}

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------
bool HttpPoolClient::authenticate(const std::string& worker_name,
                                 const std::string& password) {
    if (!is_connected()) return false;

    worker_name_ = worker_name;
    password_ = password;

    json req;
    req["worker_name"] = worker_name;
    req["version"] = "1.0.0-starminer";
    req["pool_type"] = "http";
    req["client"] = "StarMiner";
    if (!password.empty()) req["password"] = password;

    std::string response;
    if (!retry_request("POST", "/api/v1/pool/auth", req.dump(), response)) {
        std::cerr << "[StarMiner] Authentication failed" << std::endl;
        return false;
    }

    try {
        auto j = json::parse(response);
        if (j.value("auth_ok", false)) {
            if (j.contains("session_token")) {
                session_token_ = j["session_token"].get<std::string>();
            }
            std::cout << "[StarMiner] Authenticated as " << worker_name << std::endl;
            return true;
        }
    } catch (...) {
        // parse error
    }

    std::cerr << "[StarMiner] Authentication rejected: " << response << std::endl;
    return false;
}

// ---------------------------------------------------------------------------
// Work management
// ---------------------------------------------------------------------------
bool HttpPoolClient::request_work(WorkAssignment& work) {
    if (!is_connected()) return false;

    json req;
    req["worker_name"] = worker_name_;
    if (!session_token_.empty()) req["session_token"] = session_token_;

    std::string response;
    if (!retry_request("POST", "/api/v1/pool/work", req.dump(), response)) {
        return false;
    }

    try {
        auto j = json::parse(response);
        std::string pub_key_hex = j.value("public_key", "");
        std::string range_start_hex = j.value("range_start", "");
        std::string range_end_hex = j.value("range_end", "");

        if (pub_key_hex.empty() || range_start_hex.empty() || range_end_hex.empty()) {
            std::cerr << "[StarMiner] Invalid work assignment" << std::endl;
            return false;
        }

        hex_to_bytes(pub_key_hex, work.public_key, 33);
        hex_to_bytes(range_start_hex, work.range_start, 32);
        hex_to_bytes(range_end_hex, work.range_end, 32);

        work.dp_bits = j.value("dp_bits", 28);
        work.work_id = j.value("work_id", 0);
        work.puzzle_name = j.value("puzzle_name", "Puzzle #135");

        current_work_id_.store(work.work_id, std::memory_order_release);
        dp_sequence_.store(0, std::memory_order_release);

        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (work_callback_) work_callback_(work);
        }
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[StarMiner] Failed to parse work response: " << e.what() << std::endl;
        return false;
    }
}

// ---------------------------------------------------------------------------
// DP submission (queued, flushed asynchronously)
// ---------------------------------------------------------------------------
bool HttpPoolClient::submit_dp(const DistinguishedPoint& dp) {
    std::lock_guard<std::mutex> lock(dp_mutex_);
    if (dp_queue_.size() >= MAX_DP_QUEUE_SIZE) {
        return false;
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
    while (!threads_stop_.load()) {
        std::unique_lock<std::mutex> lock(dp_mutex_);
        dp_cv_.wait_for(lock, std::chrono::milliseconds(FLUSH_INTERVAL_MS),
            [this]() {
                return threads_stop_.load() || dp_queue_.size() >= batch_size_;
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

    json req;
    req["worker_name"] = worker_name_;
    if (!session_token_.empty()) req["session_token"] = session_token_;
    req["work_id"] = current_work_id_.load(std::memory_order_relaxed);

    json dps_arr = json::array();
    for (const auto& dp : batch) {
        json item;
        item["x"] = bytes_to_hex(dp.x, 32);
        item["d"] = bytes_to_hex(dp.d, 32);
        item["type"] = dp.type;
        item["sequence"] = dp_sequence_.fetch_add(1, std::memory_order_relaxed);
        item["dp_bits"] = dp.dp_bits;
        dps_arr.push_back(item);
    }
    req["dps"] = dps_arr;

    // Attach telemetry
    {
        std::lock_guard<std::mutex> lock(telemetry_mutex_);
        if (!gpu_names_.empty()) {
            json telemetry = json::object();
            for (const auto& [id, name] : gpu_names_) {
                json gpu;
                gpu["name"] = name;
                auto it = gpu_mhs_.find(id);
                if (it != gpu_mhs_.end()) gpu["mkeys_s"] = it->second;
                telemetry[std::to_string(id)] = gpu;
            }
            req["telemetry"] = telemetry;
        }
    }

    std::string response;
    if (!retry_request("POST", "/api/v1/pool/dp_batch", req.dump(), response)) {
        std::cerr << "[StarMiner] Failed to submit batch of " << batch.size() << " DPs" << std::endl;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Heartbeat loop
// ---------------------------------------------------------------------------
void HttpPoolClient::heartbeat_loop() {
    while (!threads_stop_.load()) {
        for (uint32_t i = 0; i < HEARTBEAT_INTERVAL_MS / 100 && !threads_stop_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (threads_stop_.load()) break;

        if (!is_connected() || session_token_.empty()) continue;

        json req;
        req["worker_name"] = worker_name_;
        req["session_token"] = session_token_;

        std::string response;
        do_request("POST", "/api/v1/pool/ping", req.dump(), response);
        // Ping is best-effort; we don't care about failures
    }
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------
PoolStats HttpPoolClient::get_stats() {
    if (!is_connected()) return PoolStats{};

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_stats_fetch_).count() < 2) {
            return cached_stats_;
        }
    }

    std::string endpoint = "/api/v1/pool/stats?worker=" + worker_name_;
    if (!session_token_.empty()) endpoint += "&token=" + session_token_;

    std::string response;
    if (!retry_request("GET", endpoint, "", response)) {
        return cached_stats_;
    }

    try {
        auto j = json::parse(response);
        PoolStats stats;
        stats.total_dps = j.value("total_dps", 0);
        stats.total_workers = j.value("total_workers", 0);
        stats.active_workers = j.value("active_workers", 0);
        stats.dps_per_second = j.value("dps_per_second", 0.0f);
        stats.your_share = j.value("your_share", 0.0f);
        stats.your_dps = j.value("your_dps", 0);
        stats.uptime_seconds = j.value("uptime_seconds", 0);
        stats.connected_workers = stats.active_workers;

        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            cached_stats_ = stats;
            last_stats_fetch_ = std::chrono::steady_clock::now();
        }
        return stats;
    } catch (...) {
        return cached_stats_;
    }
}

// ---------------------------------------------------------------------------
// Solution reporting
// ---------------------------------------------------------------------------
bool HttpPoolClient::report_solution(const uint8_t* private_key) {
    if (!is_connected()) return false;

    json req;
    req["worker_name"] = worker_name_;
    if (!session_token_.empty()) req["session_token"] = session_token_;
    req["private_key"] = bytes_to_hex(private_key, 32);
    req["work_id"] = current_work_id_.load(std::memory_order_relaxed);

    std::string response;
    bool ok = retry_request("POST", "/api/v1/pool/solution", req.dump(), response);

    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (solution_callback_) solution_callback_(private_key);
    }

    if (ok) {
        std::cout << "[StarMiner] Solution reported to pool!" << std::endl;
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
// HTTP request with retry
// ---------------------------------------------------------------------------
bool HttpPoolClient::retry_request(const std::string& method,
                                   const std::string& endpoint,
                                   const std::string& json_payload,
                                   std::string& response_out) {
    std::random_device rd;
    std::mt19937 rng(rd());
    uint32_t delay_ms = 1000;

    for (uint32_t attempt = 0; attempt < MAX_RETRY_ATTEMPTS; ++attempt) {
        if (do_request(method, endpoint, json_payload, response_out)) {
            return true;
        }
        if (attempt + 1 < MAX_RETRY_ATTEMPTS && !threads_stop_.load()) {
            std::uniform_int_distribution<uint32_t> jitter(delay_ms / 2, delay_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(jitter(rng)));
            delay_ms = std::min(delay_ms * 2, 30000u);
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Raw HTTP via libcurl
// ---------------------------------------------------------------------------
#ifdef COLLIDER_HAVE_CURL
bool HttpPoolClient::do_request(const std::string& method,
                                const std::string& endpoint,
                                const std::string& json_payload,
                                std::string& response_out) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string url = base_url_ + endpoint;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "User-Agent: StarMiner/1.0");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms_));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, verify_cert_ ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, verify_cert_ ? 2L : 0L);

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    } else if (method == "GET") {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return false;
    }
    return (http_code >= 200 && http_code < 300);
}
#else
bool HttpPoolClient::do_request(const std::string&, const std::string&,
                                const std::string&, std::string&) {
    return false;
}
#endif

// ---------------------------------------------------------------------------
// Hex helpers
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
