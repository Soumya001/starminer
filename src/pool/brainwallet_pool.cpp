/**
 * brainwallet_pool.cpp — Brain wallet pool client + server implementation.
 *
 * Protocol: newline-delimited JSON over plain TCP.
 * Each message is a single JSON object followed by '\n'.
 */

#include "brainwallet_pool.hpp"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   using sock_t = SOCKET;
#  define SOCK_INVALID INVALID_SOCKET
#  define SOCK_ERR     SOCKET_ERROR
#  define sock_close   closesocket
#  define sock_errno   WSAGetLastError()
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <errno.h>
   using sock_t = int;
#  define SOCK_INVALID (-1)
#  define SOCK_ERR     (-1)
#  define sock_close   close
#  define sock_errno   errno
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../tools/utxo_bloom_builder.hpp"

#if defined(STARMINER_USE_CUDA) || defined(STARMINER_USE_ROCM)
#include "../gpu/brain_wallet_gpu.hpp"
#endif

namespace starminer {
namespace pool {

// ── Tiny JSON helpers (no external dependency) ──────────────────────────────

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '"')  { out += "\\\""; }
        else if (c == '\\') { out += "\\\\"; }
        else if (c == '\n') { out += "\\n"; }
        else if (c == '\r') { out += "\\r"; }
        else               { out += c; }
    }
    return out;
}

// Build {"type":"scan","id":N,"passphrases":[...]}
static std::string make_scan_msg(uint64_t id,
                                  const std::vector<std::string>& pws)
{
    std::ostringstream o;
    o << "{\"type\":\"scan\",\"id\":" << id << ",\"passphrases\":[";
    for (size_t i = 0; i < pws.size(); ++i) {
        if (i) o << ',';
        o << '"' << json_escape(pws[i]) << '"';
    }
    o << "]}\n";
    return o.str();
}

// Build {"type":"result","id":N,"hits":[{"passphrase":"...","scheme":0},...]}
static std::string make_result_msg(uint64_t id,
                                    const std::vector<BrainWalletHitResult>& hits)
{
    std::ostringstream o;
    o << "{\"type\":\"result\",\"id\":" << id << ",\"hits\":[";
    for (size_t i = 0; i < hits.size(); ++i) {
        if (i) o << ',';
        o << "{\"passphrase\":\"" << json_escape(hits[i].passphrase)
          << "\",\"scheme\":" << hits[i].scheme_id << "}";
    }
    o << "]}\n";
    return o.str();
}

// Build {"type":"stats","checked":N,"hits":N,"rate":N}
static std::string make_stats_msg(uint64_t checked, uint64_t hits, double rate)
{
    std::ostringstream o;
    o << "{\"type\":\"stats\",\"checked\":" << checked
      << ",\"hits\":" << hits
      << ",\"rate\":" << (uint64_t)rate << "}\n";
    return o.str();
}

// Very minimal JSON field extractor — good enough for this protocol.
static std::string json_get_str(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos + key.size() + 2);
    if (pos == std::string::npos) return {};
    ++pos;
    std::string val;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            ++pos;
            if (json[pos] == 'n') val += '\n';
            else if (json[pos] == 'r') val += '\r';
            else val += json[pos];
        } else {
            val += json[pos];
        }
        ++pos;
    }
    return val;
}

static uint64_t json_get_u64(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return 0;
    pos = json.find(':', pos + key.size() + 2);
    if (pos == std::string::npos) return 0;
    while (pos < json.size() && (json[pos] == ':' || json[pos] == ' ')) ++pos;
    return (uint64_t)std::stoull(json.substr(pos));
}

static std::vector<std::string> json_get_arr_str(const std::string& json,
                                                   const std::string& key) {
    std::vector<std::string> out;
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return out;
    pos = json.find('[', pos);
    if (pos == std::string::npos) return out;
    ++pos;
    while (pos < json.size() && json[pos] != ']') {
        while (pos < json.size() && json[pos] != '"' && json[pos] != ']') ++pos;
        if (pos >= json.size() || json[pos] == ']') break;
        ++pos;
        std::string val;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                ++pos;
                if (json[pos] == 'n') val += '\n';
                else if (json[pos] == 'r') val += '\r';
                else val += json[pos];
            } else {
                val += json[pos];
            }
            ++pos;
        }
        if (pos < json.size()) ++pos;
        out.push_back(std::move(val));
    }
    return out;
}

// ── Socket helpers ────────────────────────────────────────────────────────────

static bool sock_init() {
#ifdef _WIN32
    WSADATA w;
    return WSAStartup(MAKEWORD(2,2), &w) == 0;
#else
    return true;
#endif
}

static bool sock_send_all(sock_t s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = (int)send(s, data.data() + sent, (int)(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

// Read until '\n' (one JSON message line).
static bool sock_readline(sock_t s, std::string& line_out) {
    line_out.clear();
    char c;
    while (true) {
        int n = (int)recv(s, &c, 1, 0);
        if (n <= 0) return false;
        if (c == '\n') return true;
        line_out += c;
        if (line_out.size() > 64 * 1024 * 1024) return false; // 64MB guard
    }
}

// ── Client ────────────────────────────────────────────────────────────────────

struct BrainWalletPoolClient::Impl {
    sock_t   fd       = SOCK_INVALID;
    uint64_t next_id  = 1;
};

BrainWalletPoolClient::BrainWalletPoolClient() {
    sock_init();
    impl_ = new Impl();
}
BrainWalletPoolClient::~BrainWalletPoolClient() {
    disconnect();
    delete impl_;
}

bool BrainWalletPoolClient::connect(const std::string& host, uint16_t port) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0) {
        error_ = "DNS lookup failed for " + host;
        return false;
    }
    sock_t fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == SOCK_INVALID) { freeaddrinfo(res); error_ = "socket()"; return false; }
    if (::connect(fd, res->ai_addr, (int)res->ai_addrlen) == SOCK_ERR) {
        sock_close(fd); freeaddrinfo(res);
        error_ = "connect() to " + host + ":" + port_str + " failed";
        return false;
    }
    freeaddrinfo(res);
    impl_->fd = fd;
    return true;
}

void BrainWalletPoolClient::disconnect() {
    if (impl_ && impl_->fd != SOCK_INVALID) {
        sock_close(impl_->fd);
        impl_->fd = SOCK_INVALID;
    }
}

bool BrainWalletPoolClient::connected() const {
    return impl_ && impl_->fd != SOCK_INVALID;
}

bool BrainWalletPoolClient::scan_batch(const std::vector<std::string>& passphrases,
                                        BrainWalletScanResult& result_out)
{
    if (!connected()) { error_ = "not connected"; return false; }
    uint64_t id = impl_->next_id++;
    std::string msg = make_scan_msg(id, passphrases);
    if (!sock_send_all(impl_->fd, msg)) {
        error_ = "send failed";
        disconnect();
        return false;
    }
    std::string line;
    while (sock_readline(impl_->fd, line)) {
        auto type = json_get_str(line, "type");
        if (type == "result") {
            result_out.batch_id = json_get_u64(line, "id");
            // Parse hits
            result_out.hits.clear();
            auto pos = line.find("\"hits\"");
            if (pos != std::string::npos) {
                auto arr_start = line.find('[', pos);
                auto arr_end   = line.rfind(']');
                if (arr_start != std::string::npos && arr_end > arr_start) {
                    std::string arr = line.substr(arr_start + 1, arr_end - arr_start - 1);
                    // Each hit is {"passphrase":"...","scheme":N}
                    size_t p = 0;
                    while (p < arr.size()) {
                        auto q = arr.find('{', p);
                        if (q == std::string::npos) break;
                        auto r = arr.find('}', q);
                        if (r == std::string::npos) break;
                        std::string obj = arr.substr(q, r - q + 1);
                        BrainWalletHitResult h;
                        h.passphrase = json_get_str(obj, "passphrase");
                        auto sp = obj.find("\"scheme\"");
                        if (sp != std::string::npos) {
                            auto cp = obj.find(':', sp);
                            if (cp != std::string::npos)
                                h.scheme_id = std::stoi(obj.substr(cp + 1));
                        }
                        if (!h.passphrase.empty())
                            result_out.hits.push_back(std::move(h));
                        p = r + 1;
                    }
                }
            }
            return true;
        }
        if (type == "stats") continue; // discard mid-stream stats
        if (type == "ping") {
            sock_send_all(impl_->fd, "{\"type\":\"pong\"}\n");
            continue;
        }
    }
    error_ = "connection closed while waiting for result";
    disconnect();
    return false;
}

// ── Server ────────────────────────────────────────────────────────────────────

struct BrainWalletPoolServer::Impl {
    sock_t               listen_fd  = SOCK_INVALID;
    std::atomic<bool>    running{false};
    std::thread          accept_thread;
    std::mutex           stats_mutex;
    uint64_t             total_checked = 0;
    uint64_t             total_hits    = 0;
    BrainWalletServerConfig cfg;
    std::function<void(const std::string&, int)> on_hit;

    // GPU pipeline (shared across all client sessions)
#if defined(STARMINER_USE_CUDA) || defined(STARMINER_USE_ROCM)
    std::unique_ptr<gpu::MultiGPUBrainWallet> pipeline;
    std::mutex pipeline_mutex;   // serialize GPU batches
#endif
};

static void handle_client(sock_t client_fd,
                           BrainWalletPoolServer::Impl* srv)
{
    std::string line;
    while (sock_readline(client_fd, line)) {
        auto type = json_get_str(line, "type");

        if (type == "ping") {
            sock_send_all(client_fd, "{\"type\":\"pong\"}\n");
            continue;
        }

        if (type == "scan") {
            uint64_t id = json_get_u64(line, "id");
            auto passphrases = json_get_arr_str(line, "passphrases");

            std::vector<BrainWalletHitResult> hits;

#if defined(STARMINER_USE_CUDA) || defined(STARMINER_USE_ROCM)
            if (srv->pipeline) {
                std::lock_guard<std::mutex> lk(srv->pipeline_mutex);
                auto result = srv->pipeline->process_batch(passphrases);
                for (const auto& h : result.hits)
                    hits.push_back({h.passphrase, h.scheme_id});
            }
#endif

            {
                std::lock_guard<std::mutex> lk(srv->stats_mutex);
                srv->total_checked += passphrases.size();
                srv->total_hits    += hits.size();
            }

            for (const auto& h : hits) {
                if (srv->on_hit) srv->on_hit(h.passphrase, h.scheme_id);
            }

            auto reply = make_result_msg(id, hits);
            if (!sock_send_all(client_fd, reply)) break;

            // Send stats every batch
            {
                std::lock_guard<std::mutex> lk(srv->stats_mutex);
                auto stats = make_stats_msg(srv->total_checked, srv->total_hits, 0);
                sock_send_all(client_fd, stats);
            }
        }
    }
    sock_close(client_fd);
}

BrainWalletPoolServer::BrainWalletPoolServer() {
    sock_init();
    impl_ = new Impl();
}
BrainWalletPoolServer::~BrainWalletPoolServer() { stop(); delete impl_; }

bool BrainWalletPoolServer::start(const BrainWalletServerConfig& cfg)
{
    impl_->cfg    = cfg;
    impl_->on_hit = on_hit;

#if defined(STARMINER_USE_CUDA) || defined(STARMINER_USE_ROCM)
    // Load bloom filter
    std::cout << "[BW-Server] Loading bloom filter: " << cfg.bloom_file << "\n";
    starminer::utxo::UTXOBloomBuilder bloom(
        starminer::utxo::UTXOBloomBuilder::Config{});
    try {
        bloom = starminer::utxo::UTXOBloomBuilder::load(cfg.bloom_file);
    } catch (const std::exception& e) {
        error_ = std::string("bloom load: ") + e.what();
        return false;
    }

    // Init GPU pipeline
    gpu::MultiGPUBrainWallet::Config pcfg;
    pcfg.batch_size = cfg.batch_size;
    impl_->pipeline = std::make_unique<gpu::MultiGPUBrainWallet>(pcfg);
    std::cout << "[BW-Server] Initializing GPU...\n";
    if (!impl_->pipeline->init()) {
        error_ = "GPU pipeline init failed";
        return false;
    }
    const auto& bd = bloom.data();
    if (!impl_->pipeline->load_bloom_filter(bd.data(), bd.size(),
                                             bloom.num_bits(),
                                             bloom.num_hashes(), 42u)) {
        error_ = "bloom upload failed";
        return false;
    }
    std::cout << "[BW-Server] Bloom filter loaded ("
              << bloom.elements_added() << " addresses)\n";
#else
    error_ = "GPU backend required (CUDA or ROCm)";
    return false;
#endif

    // Bind listen socket
    impl_->listen_fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (impl_->listen_fd == SOCK_INVALID) {
        // Fallback to IPv4
        impl_->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (impl_->listen_fd == SOCK_INVALID) { error_ = "socket()"; return false; }
        sockaddr_in addr4{};
        addr4.sin_family      = AF_INET;
        addr4.sin_port        = htons(cfg.port);
        addr4.sin_addr.s_addr = INADDR_ANY;
        int on = 1;
        setsockopt(impl_->listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   (const char*)&on, sizeof(on));
        if (bind(impl_->listen_fd, (sockaddr*)&addr4, sizeof(addr4)) == SOCK_ERR) {
            error_ = "bind() failed on port " + std::to_string(cfg.port);
            return false;
        }
    } else {
        int off = 0;
        setsockopt(impl_->listen_fd, IPPROTO_IPV6, IPV6_V6ONLY,
                   (const char*)&off, sizeof(off));
        sockaddr_in6 addr6{};
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port   = htons(cfg.port);
        int on = 1;
        setsockopt(impl_->listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   (const char*)&on, sizeof(on));
        if (bind(impl_->listen_fd, (sockaddr*)&addr6, sizeof(addr6)) == SOCK_ERR) {
            sock_close(impl_->listen_fd);
            impl_->listen_fd = SOCK_INVALID;
            error_ = "bind() failed on port " + std::to_string(cfg.port);
            return false;
        }
    }

    if (listen(impl_->listen_fd, cfg.max_clients) == SOCK_ERR) {
        error_ = "listen() failed";
        return false;
    }

    impl_->running = true;
    impl_->accept_thread = std::thread([this]() {
        while (impl_->running) {
            sockaddr_storage client_addr{};
            socklen_t addrlen = sizeof(client_addr);
            sock_t cfd = accept(impl_->listen_fd,
                                (sockaddr*)&client_addr, &addrlen);
            if (cfd == SOCK_INVALID) break;
            std::cout << "[BW-Server] Client connected\n";
            auto* srv = impl_;
            std::thread([cfd, srv]() {
                handle_client(cfd, srv);
                std::cout << "[BW-Server] Client disconnected\n";
            }).detach();
        }
    });

    std::cout << "[BW-Server] Listening on port " << cfg.port << "\n";
    return true;
}

void BrainWalletPoolServer::stop() {
    if (!impl_) return;
    impl_->running = false;
    if (impl_->listen_fd != SOCK_INVALID) {
        sock_close(impl_->listen_fd);
        impl_->listen_fd = SOCK_INVALID;
    }
    if (impl_->accept_thread.joinable())
        impl_->accept_thread.join();
}

bool BrainWalletPoolServer::running() const {
    return impl_ && impl_->running;
}

}  // namespace pool
}  // namespace starminer
