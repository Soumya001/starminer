/**
 * brain_wallet_runner.cpp - Brain wallet scanning mode.
 *
 * Reads passphrases from a wordlist, hashes them on GPU
 * (SHA-256 -> secp256k1 -> RIPEMD-160), checks resulting addresses
 * against a bloom filter of funded Bitcoin addresses.
 *
 * GPU: CUDA (full), Metal (full). ROCm brain wallet not supported due to
 * HIP device linker limitations with extern __device__ symbols.
 */

#include "runtime/brain_wallet_runner.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/crypto_cpu.hpp"
#include "runtime/runtime_globals.hpp"
#include "tools/utxo_bloom_builder.hpp"
#include "ui/box_render.hpp"

#if defined(STARMINER_USE_CUDA)
#include "gpu/brain_wallet_gpu.hpp"
#elif defined(STARMINER_USE_METAL)
#include "gpu/brain_wallet_metal.hpp"
#endif

namespace starminer {
namespace runtime {

static std::string bw_format_rate(double r) {
    if (r >= 1e9) return std::to_string((uint64_t)(r / 1e9)) + " GH/s";
    if (r >= 1e6) return std::to_string((uint64_t)(r / 1e6)) + " MH/s";
    if (r >= 1e3) return std::to_string((uint64_t)(r / 1e3)) + " KH/s";
    return std::to_string((uint64_t)r) + " H/s";
}

static void record_hit(const std::string& passphrase, int scheme_id,
                       std::ofstream& hitlog, uint64_t& total_hits)
{
    ++total_hits;
    std::cout << "\n[HIT] " << passphrase << "  scheme=" << scheme_id << "\n";
    if (hitlog) {
        hitlog << passphrase << "\t" << scheme_id << "\n";
        hitlog.flush();
    }
}

static void print_progress(uint64_t checked, uint64_t hits,
                           const std::chrono::steady_clock::time_point& t_start)
{
    double s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t_start).count();
    std::cout << "\r[*] Checked: " << checked
              << "  Rate: " << bw_format_rate(checked / (s > 0 ? s : 1))
              << "  Hits: " << hits << "     " << std::flush;
}

static void print_summary(uint64_t checked, uint64_t hits,
                          const std::chrono::steady_clock::time_point& t_start)
{
    namespace boxui = ::starminer::ui::box;
    double s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t_start).count();
    std::cout << "\n\n";
    boxui::top(std::cout);
    boxui::centered(std::cout, "SCAN COMPLETE");
    boxui::top(std::cout);
    boxui::kv(std::cout, "Total checked", std::to_string(checked));
    boxui::kv(std::cout, "Total hits",    std::to_string(hits));
    {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << s << "s  ("
           << bw_format_rate(s > 0 ? checked / s : 0) << ")";
        boxui::kv(std::cout, "Time / Rate", ss.str());
    }
    if (hits > 0) boxui::kv(std::cout, "Hit log", "brainwallet_hits.txt");
    boxui::bottom(std::cout);
    std::cout << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
int run_brain_wallet_mode(const Arguments& args)
{
    namespace boxui = ::starminer::ui::box;

    if (args.bloom_file.empty()) {
        std::cerr << "[!] Requires --bloom <funded.blf>\n";
        return 1;
    }
    if (args.wordlist_file.empty()) {
        std::cerr << "[!] Requires --wordlist <passphrases.txt>\n";
        return 1;
    }

    // Load bloom filter
    std::cout << "[*] Loading bloom filter: " << args.bloom_file << "\n";
    starminer::utxo::UTXOBloomBuilder bloom(
        starminer::utxo::UTXOBloomBuilder::Config{});
    try {
        bloom = starminer::utxo::UTXOBloomBuilder::load(args.bloom_file);
    } catch (const std::exception& e) {
        std::cerr << "[!] " << e.what() << "\n";
        return 1;
    }
    const auto& bdata = bloom.data();
    std::cout << "[*] " << bloom.elements_added() << " addresses, "
              << (bdata.size() / 1024 / 1024) << " MB\n";

    // Open wordlist
    std::ifstream wl(args.wordlist_file);
    if (!wl) {
        std::cerr << "[!] Cannot open: " << args.wordlist_file << "\n";
        return 1;
    }

    const int batch_size = (args.batch_size > 0) ? args.batch_size : 65536;

    boxui::top(std::cout);
    boxui::centered(std::cout, "BRAIN WALLET SCANNER");
    boxui::top(std::cout);
    boxui::kv(std::cout, "Wordlist",     args.wordlist_file);
    boxui::kv(std::cout, "Bloom filter", args.bloom_file);
    boxui::kv(std::cout, "Batch size",   std::to_string(batch_size));
    boxui::bottom(std::cout);
    std::cout << "\n";

    std::ofstream hitlog("brainwallet_hits.txt", std::ios::app);
    uint64_t total_checked = 0, total_hits = 0;
    const auto t_start   = std::chrono::steady_clock::now();
    auto       t_last    = t_start;

#if defined(STARMINER_USE_CUDA)
    {
        gpu::MultiGPUBrainWallet::Config cfg;
        cfg.gpu_ids    = args.gpu_ids;
        cfg.batch_size = batch_size;

        gpu::MultiGPUBrainWallet pipeline(cfg);
        std::cout << "[*] Initializing GPU...\n";
        if (!pipeline.init()) {
            std::cerr << "[!] GPU init failed.\n";
            return 1;
        }
        if (!pipeline.load_bloom_filter(bdata.data(), bdata.size(),
                                        bloom.num_bits(), bloom.num_hashes(), 42u)) {
            std::cerr << "[!] Bloom upload failed.\n";
            return 1;
        }
        std::cout << "[*] GPU ready\n\n";

        std::vector<std::string> batch;
        batch.reserve(batch_size);
        std::string line;

        auto flush = [&]() {
            if (batch.empty()) return;
            auto r = pipeline.process_batch(batch);
            total_checked += r.processed;
            for (const auto& h : r.hits)
                record_hit(h.passphrase, h.scheme_id, hitlog, total_hits);
            batch.clear();
        };

        while (!g_shutdown && std::getline(wl, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || (int)line.size() > 256) continue;
            batch.push_back(line);
            if ((int)batch.size() >= batch_size) flush();
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - t_last).count() >= 1.0) {
                print_progress(total_checked, total_hits, t_start);
                t_last = now;
            }
        }
        flush();
    }

#elif defined(STARMINER_USE_METAL)
    {
        gpu::MetalBrainWalletSolver solver;
        std::cout << "[*] Initializing Metal GPU...\n";
        if (!solver.init()) {
            std::cerr << "[!] Metal init failed: " << solver.error() << "\n";
            return 1;
        }
        if (!solver.load_bloom_filter(bdata.data(), bdata.size(),
                                      bloom.num_bits(), bloom.num_hashes(), 42u)) {
            std::cerr << "[!] Bloom upload failed.\n";
            return 1;
        }
        std::cout << "[*] GPU ready\n\n";

        std::vector<std::string> batch;
        batch.reserve(batch_size);
        std::string line;

        auto flush = [&]() {
            if (batch.empty()) return;
            std::vector<gpu::MetalBrainWalletHit> hits;
            solver.process_batch(batch, hits);
            total_checked += batch.size();
            for (const auto& h : hits)
                record_hit(h.passphrase, h.scheme_id, hitlog, total_hits);
            batch.clear();
        };

        while (!g_shutdown && std::getline(wl, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || (int)line.size() > 256) continue;
            batch.push_back(line);
            if ((int)batch.size() >= batch_size) flush();
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - t_last).count() >= 1.0) {
                print_progress(total_checked, total_hits, t_start);
                t_last = now;
            }
        }
        flush();
    }

#else
    std::cerr << "[!] Brain wallet requires a GPU build (CUDA, ROCm, or Metal).\n";
    return 1;
#endif

    print_summary(total_checked, total_hits, t_start);
    return 0;
}

// ── Range scan ────────────────────────────────────────────────────────────────

static std::string bytes_to_hex(const uint8_t* b, size_t n) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { out += hex[b[i] >> 4]; out += hex[b[i] & 0xf]; }
    return out;
}

static bool bloom_probe(const starminer::utxo::UTXOBloomBuilder& bf,
                        const uint8_t h160[20])
{
    auto murmur3 = [](const uint8_t* d, uint32_t seed) -> uint32_t {
        const uint32_t c1 = 0xcc9e2d51u, c2 = 0x1b873593u;
        uint32_t h = seed;
        for (int i = 0; i < 5; ++i) {
            uint32_t k = (uint32_t)d[i*4] | ((uint32_t)d[i*4+1]<<8)
                       | ((uint32_t)d[i*4+2]<<16) | ((uint32_t)d[i*4+3]<<24);
            k *= c1; k = (k<<15)|(k>>17); k *= c2;
            h ^= k; h = (h<<13)|(h>>19); h = h*5u+0xe6546b64u;
        }
        h ^= 20u;
        h ^= h>>16; h *= 0x85ebca6bu; h ^= h>>13; h *= 0xc2b2ae35u; h ^= h>>16;
        return h;
    };
    uint64_t run = murmur3(h160, 42u), step = murmur3(h160, 42u ^ 0x5f3759dfu);
    uint64_t nb = bf.num_bits();
    const auto& bits = bf.data();
    for (uint32_t i = 0; i < bf.num_hashes(); ++i) {
        uint64_t idx = run % nb;
        if (!(bits[idx/8] & (1u << (idx & 7)))) return false;
        run += step;
    }
    return true;
}

int run_range_scan(const Arguments& args)
{
    namespace boxui = ::starminer::ui::box;

    if (args.bloom_file.empty()) {
        std::cerr << "[!] --range-scan requires --bloom <file.blf>\n";
        return 1;
    }

    const int min_bits = args.range_scan_min_bits < 1  ? 1   : args.range_scan_min_bits;
    const int max_bits = args.range_scan_max_bits > 160 ? 160 : args.range_scan_max_bits;

    std::cout << "[*] Loading bloom filter: " << args.bloom_file << "\n";
    starminer::utxo::UTXOBloomBuilder bloom(
        starminer::utxo::UTXOBloomBuilder::Config{});
    try { bloom = starminer::utxo::UTXOBloomBuilder::load(args.bloom_file); }
    catch (const std::exception& e) { std::cerr << "[!] " << e.what() << "\n"; return 1; }

    boxui::top(std::cout);
    boxui::centered(std::cout, "RANGE SCAN");
    boxui::top(std::cout);
    boxui::kv(std::cout, "Addresses", std::to_string(bloom.elements_added()));
    boxui::kv(std::cout, "Bit range",
              std::to_string(min_bits) + " – " + std::to_string(max_bits));
    if (max_bits > 42)
        boxui::kv(std::cout, "Note", "Ranges > 42 bits are slow on CPU");
    boxui::bottom(std::cout);
    std::cout << "\n";

    std::ofstream hitlog("range_scan_hits.txt", std::ios::app);
    uint64_t total_checked = 0, total_hits = 0;
    const auto t_start = std::chrono::steady_clock::now();
    auto t_last = t_start;

    for (int bits = min_bits; bits <= max_bits && !g_shutdown; ++bits) {
        cpu::uint256_t k{}, end{};
        // k = 2^(bits-1)
        if (bits == 1) { k.d[0] = 1; }
        else { k.d[(bits-1)/64] = (uint64_t)1 << ((bits-1)%64); }
        // end = 2^bits - 1
        if (bits % 64 == 0) {
            for (int i = 0; i < bits/64; ++i) end.d[i] = ~uint64_t(0);
        } else {
            end.d[bits/64] = ((uint64_t)1 << (bits%64)) - 1;
            for (int i = 0; i < bits/64; ++i) end.d[i] = ~uint64_t(0);
        }

        uint64_t range_size = bits <= 63 ? ((uint64_t)1 << (bits-1)) : UINT64_MAX;
        std::cout << "[*] " << bits << "-bit range (" << range_size << " keys)...\n";

        while (!g_shutdown) {
            bool done = (k.d[3] > end.d[3]) ||
                (k.d[3]==end.d[3] && k.d[2] > end.d[2]) ||
                (k.d[3]==end.d[3] && k.d[2]==end.d[2] && k.d[1] > end.d[1]) ||
                (k.d[3]==end.d[3] && k.d[2]==end.d[2] && k.d[1]==end.d[1] && k.d[0] > end.d[0]);
            if (done) break;

            uint8_t kb[32];
            for (int i = 0; i < 4; ++i) {
                uint64_t l = k.d[3-i];
                for (int j = 0; j < 8; ++j) kb[i*8+j] = (uint8_t)(l >> (56-8*j));
            }

            auto h160 = cpu::compute_hash160(kb);
            ++total_checked;

            if (bloom_probe(bloom, h160.data())) {
                ++total_hits;
                std::string priv = bytes_to_hex(kb, 32);
                std::string addr = bytes_to_hex(h160.data(), 20);
                std::cout << "\n[HIT] bits=" << bits
                          << " priv=0x" << priv << " h160=" << addr << "\n";
                if (hitlog) { hitlog << bits << "\t0x" << priv << "\t" << addr << "\n"; hitlog.flush(); }
            }

            uint64_t carry = 1;
            for (int i = 0; i < 4 && carry; ++i) { k.d[i] += carry; carry = k.d[i]==0 ? 1 : 0; }

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - t_last).count() >= 1.0) {
                double s = std::chrono::duration<double>(now - t_start).count();
                std::cout << "\r[*] bits=" << bits << " checked=" << total_checked
                          << " rate=" << bw_format_rate(total_checked/(s>0?s:1))
                          << " hits=" << total_hits << "     " << std::flush;
                t_last = now;
            }
        }
    }

    print_summary(total_checked, total_hits, t_start);
    return 0;
}

}  // namespace runtime
}  // namespace starminer
