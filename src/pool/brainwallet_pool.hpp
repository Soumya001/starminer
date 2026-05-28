#pragma once
/**
 * Brain Wallet Pool Protocol
 *
 * Simple TCP JSON-line protocol for distributed brain wallet scanning.
 *
 * Server holds the bloom filter and GPU pipeline.
 * Clients send passphrase batches; server returns hits.
 *
 * Protocol (newline-delimited JSON):
 *   Client → Server:
 *     {"type":"scan","id":N,"passphrases":["word1","word2",...]}
 *   Server → Client:
 *     {"type":"result","id":N,"hits":[{"passphrase":"word","scheme":0},...]}
 *   Server → Client (periodic):
 *     {"type":"stats","checked":N,"hits":N,"rate":N}
 *   Either direction:
 *     {"type":"ping"}
 *     {"type":"pong"}
 *
 * Default port: 17404
 */

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace starminer {
namespace pool {

constexpr uint16_t BRAINWALLET_POOL_DEFAULT_PORT = 17404;

struct BrainWalletHitResult {
    std::string passphrase;
    int         scheme_id = 0;
};

struct BrainWalletScanResult {
    uint64_t                         batch_id = 0;
    std::vector<BrainWalletHitResult> hits;
};

// ── Client API ────────────────────────────────────────────────────────────────
class BrainWalletPoolClient {
public:
    BrainWalletPoolClient();
    ~BrainWalletPoolClient();

    BrainWalletPoolClient(const BrainWalletPoolClient&) = delete;
    BrainWalletPoolClient& operator=(const BrainWalletPoolClient&) = delete;

    bool connect(const std::string& host, uint16_t port);
    void disconnect();
    bool connected() const;

    // Send a batch and receive the results synchronously.
    bool scan_batch(const std::vector<std::string>& passphrases,
                    BrainWalletScanResult& result_out);

    const std::string& error() const { return error_; }

private:
    struct Impl;
    Impl*       impl_ = nullptr;
    std::string error_;
};

// ── Server API ────────────────────────────────────────────────────────────────
struct BrainWalletServerConfig {
    uint16_t    port        = BRAINWALLET_POOL_DEFAULT_PORT;
    std::string bloom_file;
    int         batch_size  = 65536;
    int         max_clients = 64;
};

class BrainWalletPoolServer {
public:
    BrainWalletPoolServer();
    ~BrainWalletPoolServer();

    BrainWalletPoolServer(const BrainWalletPoolServer&) = delete;
    BrainWalletPoolServer& operator=(const BrainWalletPoolServer&) = delete;

    bool start(const BrainWalletServerConfig& cfg);
    void stop();
    bool running() const;

    // Called for each hit found (any client). Optional logging hook.
    std::function<void(const std::string& passphrase, int scheme)> on_hit;

    const std::string& error() const { return error_; }

private:
    struct Impl;
    Impl*       impl_ = nullptr;
    std::string error_;
};

}  // namespace pool
}  // namespace starminer
