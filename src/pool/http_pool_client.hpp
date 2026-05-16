// http_pool_client.hpp - HTTP/JSON pool client for StarMiner
// Connects to custom pool servers over HTTP/HTTPS using JSON/REST

#pragma once

#include "pool_client.hpp"
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <map>

namespace collider {
namespace pool {

/**
 * HTTP/JSON Pool Client
 *
 * Connects to pool servers over HTTP/HTTPS using JSON/REST.
 * Features: async DP batching, heartbeat, exponential backoff retry,
 * per-GPU telemetry, proper JSON parsing via nlohmann/json.
 *
 * Usage: --pool https://starnetlive.space --worker <btc_address>
 */
class HttpPoolClient : public PoolClient {
public:
    static constexpr size_t MAX_DP_QUEUE_SIZE = 100000;
    static constexpr size_t DEFAULT_BATCH_SIZE = 500;
    static constexpr uint32_t FLUSH_INTERVAL_MS = 5000;
    static constexpr uint32_t HEARTBEAT_INTERVAL_MS = 30000;
    static constexpr uint32_t MAX_RETRY_ATTEMPTS = 5;

    HttpPoolClient();
    ~HttpPoolClient() override;

    // PoolClient interface
    bool connect(const std::string& host, uint16_t port) override;
    void disconnect() override;
    bool is_connected() const override;

    bool authenticate(const std::string& worker_name,
                     const std::string& password = "") override;

    bool request_work(WorkAssignment& work) override;
    bool submit_dp(const DistinguishedPoint& dp) override;
    bool submit_dps(const std::vector<DistinguishedPoint>& dps) override;

    PoolStats get_stats() override;
    bool report_solution(const uint8_t* private_key) override;

    void set_solution_callback(SolutionCallback cb) override;
    void set_work_callback(WorkCallback cb) override;

    std::string get_pool_type() const override { return "http"; }

    // Configuration
    void set_timeout(uint32_t timeout_ms) { timeout_ms_ = timeout_ms; }
    void set_use_tls(bool use_tls) { use_tls_ = use_tls; }
    void set_verify_cert(bool verify) { verify_cert_ = verify; }
    void set_batch_size(size_t size) { batch_size_ = size; }

    // Telemetry
    void set_gpu_telemetry(const std::map<int, std::string>& gpu_names,
                           const std::map<int, double>& gpu_mhs);

private:
    std::string base_url_;
    std::string worker_name_;
    std::string session_token_;
    std::string password_;

    uint32_t timeout_ms_ = 30000;
    bool use_tls_ = true;
    bool verify_cert_ = true;
    size_t batch_size_ = DEFAULT_BATCH_SIZE;

    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> dp_sequence_{0};
    std::atomic<uint64_t> current_work_id_{0};

    // Telemetry
    std::mutex telemetry_mutex_;
    std::map<int, std::string> gpu_names_;
    std::map<int, double> gpu_mhs_;

    // DP batching
    std::mutex dp_mutex_;
    std::condition_variable dp_cv_;
    std::queue<DistinguishedPoint> dp_queue_;

    // Background threads
    std::thread flush_thread_;
    std::thread heartbeat_thread_;
    std::atomic<bool> threads_stop_{false};

    // Stats caching
    mutable std::mutex stats_mutex_;
    PoolStats cached_stats_;
    std::chrono::steady_clock::time_point last_stats_fetch_;

    // Callbacks
    std::mutex callback_mutex_;
    SolutionCallback solution_callback_;
    WorkCallback work_callback_;

    // Internal helpers
    void flush_loop();
    void heartbeat_loop();
    bool flush_batch();
    bool do_request(const std::string& method, const std::string& endpoint,
                    const std::string& json_payload, std::string& response_out);
    bool retry_request(const std::string& method, const std::string& endpoint,
                       const std::string& json_payload, std::string& response_out);

    std::string bytes_to_hex(const uint8_t* data, size_t len) const;
    bool hex_to_bytes(const std::string& hex, uint8_t* out, size_t out_len) const;
};

} // namespace pool
} // namespace collider
