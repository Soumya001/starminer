/**
 * brain_wallet_runner.cpp - Brain wallet scanning mode.
 *
 * Reads passphrases from a wordlist, hashes them on GPU, checks the
 * resulting addresses against a bloom filter of funded Bitcoin addresses.
 * Supports CUDA, ROCm (HIP), and Apple Metal backends.
 */

#include "runtime/brain_wallet_runner.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "core/crypto_cpu.hpp"
#include "runtime/runtime_globals.hpp"
#include "tools/utxo_bloom_builder.hpp"
#include "ui/box_render.hpp"

#if defined(STARMINER_USE_CUDA) || defined(STARMINER_USE_ROCM)
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

// ── Common: load bloom filter + open wordlist ─────────────────────────────
static bool load_bloom(const std::string& path,
                       starminer::utxo::UTXOBloomBuilder& out)
{
    try {
        out = starminer::utxo::UTXOBloomBuilder::load(path);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[!] Failed to load bloom filter: " << e.what() << "\n";
        return false;
    }
}

// ── Common: process hits (print + log) ────────────────────────────────────
static void record_hit(const std::string& passphrase, int scheme_id,
                       std::ofstream& hitlog, uint64_t& total_hits)
{
    ++total_hits;
    std::cout << "\n[HIT] Passphrase: " << passphrase
              << "  scheme=" << scheme_id << "\n";
    if (hitlog) {
        hitlog << passphrase << "\t" << scheme_id << "\n";
        hitlog.flush();
    }
}

// ── Common: progress line ─────────────────────────────────────────────────
static void print_progress(uint64_t checked, uint64_t hits,
                           const std::chrono::steady_clock::time_point& t_start)
{
    double total_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start)
            .count();
    double rate = total_s > 0 ? checked / total_s : 0;
    std::cout << "\r[*] Checked: " << checked
              << "  Rate: " << bw_format_rate(rate)
              << "  Hits: " << hits
              << "     " << std::flush;
}

// ── Common: print summary ─────────────────────────────────────────────────
static void print_summary(uint64_t checked, uint64_t hits,
                          const std::chrono::steady_clock::time_point& t_start)
{
    namespace boxui = ::starminer::ui::box;
    double total_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start)
            .count();
    std::cout << "\n\n";
    boxui::top(std::cout);
    boxui::centered(std::cout, "SCAN COMPLETE");
    boxui::top(std::cout);
    boxui::kv(std::cout, "Total checked", std::to_string(checked));
    boxui::kv(std::cout, "Total hits", std::to_string(hits));
    {
        std::ostringstream s;
        s << std::fixed << std::setprecision(1) << total_s << "s  ("
          << bw_format_rate(total_s > 0 ? checked / total_s : 0) << ")";
        boxui::kv(std::cout, "Time / Rate", s.str());
    }
    if (hits > 0)
        boxui::kv(std::cout, "Hit log", "brainwallet_hits.txt");
    boxui::bottom(std::cout);
    std::cout << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
int run_brain_wallet_mode(const Arguments& args)
{
    namespace boxui = ::starminer::ui::box;

    if (args.bloom_file.empty()) {
        std::cerr << "[!] Brain wallet mode requires a bloom filter.\n"
                  << "    Use: --bloom funded_addresses.blf\n";
        return 1;
    }
    if (args.wordlist_file.empty()) {
        std::cerr << "[!] Brain wallet mode requires a wordlist.\n"
                  << "    Use: --wordlist passphrases.txt\n";
        return 1;
    }

    // ── Load bloom filter ─────────────────────────────────────────────────
    std::cout << "[*] Loading bloom filter: " << args.bloom_file << "\n";
    starminer::utxo::UTXOBloomBuilder bloom_filter(
        starminer::utxo::UTXOBloomBuilder::Config{});
    if (!load_bloom(args.bloom_file, bloom_filter)) return 1;

    const auto& bdata = bloom_filter.data();
    std::cout << "[*] Bloom filter: "
              << bloom_filter.elements_added() << " addresses, "
              << (bdata.size() / 1024 / 1024) << " MB, "
              << bloom_filter.num_hashes() << " hash functions\n";

    // ── Open wordlist ─────────────────────────────────────────────────────
    std::ifstream wl(args.wordlist_file);
    if (!wl) {
        std::cerr << "[!] Cannot open wordlist: " << args.wordlist_file << "\n";
        return 1;
    }

    const int batch_size = (args.batch_size > 0) ? args.batch_size : 65536;

    // ── Banner ────────────────────────────────────────────────────────────
    boxui::top(std::cout);
    boxui::centered(std::cout, "BRAIN WALLET SCANNER");
    boxui::top(std::cout);
    boxui::kv(std::cout, "Wordlist", args.wordlist_file);
    boxui::kv(std::cout, "Bloom filter", args.bloom_file);
    boxui::kv(std::cout, "Batch size", std::to_string(batch_size));
    boxui::bottom(std::cout);
    std::cout << "\n";

    // ── Hit log ───────────────────────────────────────────────────────────
    std::ofstream hitlog("brainwallet_hits.txt", std::ios::app);
    uint64_t total_checked = 0, total_hits = 0;
    const auto t_start = std::chrono::steady_clock::now();
    auto t_last_print  = t_start;

#if defined(STARMINER_USE_CUDA) || defined(STARMINER_USE_ROCM)
    // ── CUDA / ROCm path ─────────────────────────────────────────────────
    {
        gpu::MultiGPUBrainWallet::Config cfg;
        cfg.gpu_ids    = args.gpu_ids;
        cfg.batch_size = batch_size;

        gpu::MultiGPUBrainWallet pipeline(cfg);
        std::cout << "[*] Initializing GPU pipeline (CUDA/ROCm)...\n";
        if (!pipeline.init()) {
            std::cerr << "[!] GPU pipeline init failed. Check GPU and drivers.\n";
            return 1;
        }
        if (!pipeline.load_bloom_filter(bdata.data(), bdata.size(),
                                        bloom_filter.num_bits(),
                                        bloom_filter.num_hashes(), 42u)) {
            std::cerr << "[!] Failed to upload bloom filter to GPU.\n";
            return 1;
        }
        std::cout << "[*] Bloom filter uploaded to GPU\n\n";

        std::vector<std::string> batch;
        batch.reserve(batch_size);
        std::string line;

        auto flush_batch = [&]() {
            if (batch.empty()) return;
            auto result = pipeline.process_batch(batch);
            total_checked += result.processed;
            for (const auto& hit : result.hits)
                record_hit(hit.passphrase, hit.scheme_id, hitlog, total_hits);
            batch.clear();
        };

        while (!g_shutdown && std::getline(wl, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || (int)line.size() > 256) continue;
            batch.push_back(line);
            if ((int)batch.size() >= batch_size) flush_batch();

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - t_last_print).count() >= 1.0) {
                print_progress(total_checked, total_hits, t_start);
                t_last_print = now;
            }
        }
        flush_batch();
    }

#elif defined(STARMINER_USE_METAL)
    // ── Apple Metal path ─────────────────────────────────────────────────
    {
        gpu::MetalBrainWalletSolver solver;
        std::cout << "[*] Initializing Metal brain wallet pipeline...\n";
        if (!solver.init()) {
            std::cerr << "[!] Metal init failed: " << solver.error() << "\n";
            return 1;
        }
        if (!solver.load_bloom_filter(bdata.data(), bdata.size(),
                                      bloom_filter.num_bits(),
                                      bloom_filter.num_hashes(), 42u)) {
            std::cerr << "[!] Failed to upload bloom filter to Metal GPU.\n";
            return 1;
        }
        std::cout << "[*] Bloom filter uploaded to Metal GPU\n\n";

        std::vector<std::string> batch;
        batch.reserve(batch_size);
        std::string line;

        auto flush_batch = [&]() {
            if (batch.empty()) return;
            std::vector<gpu::MetalBrainWalletHit> hits;
            solver.process_batch(batch, hits);
            total_checked += batch.size();
            for (const auto& hit : hits)
                record_hit(hit.passphrase, hit.scheme_id, hitlog, total_hits);
            batch.clear();
        };

        while (!g_shutdown && std::getline(wl, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || (int)line.size() > 256) continue;
            batch.push_back(line);
            if ((int)batch.size() >= batch_size) flush_batch();

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - t_last_print).count() >= 1.0) {
                print_progress(total_checked, total_hits, t_start);
                t_last_print = now;
            }
        }
        flush_batch();
    }

#else
    // ── CPU-only build ────────────────────────────────────────────────────
    std::cerr << "[!] Brain wallet mode requires a GPU build (CUDA, ROCm, or Metal).\n";
    return 1;
#endif

    print_summary(total_checked, total_hits, t_start);
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Range scan: sweep raw private key ranges against bloom filter.
// For each bit width N in [min_bits, max_bits]:
//   - Range: k in [2^(N-1), 2^N - 1]
//   - Derive address for each k, check bloom filter
//   - Save any funded wallet found
//
// Uses CPU secp256k1 for portability. GPU acceleration is future work.
// Practical limit: ~2^36 keys before runtimes become multi-day.
// ─────────────────────────────────────────────────────────────────────────────
static std::string bytes_to_hex(const uint8_t* b, size_t n) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out += hex[b[i] >> 4];
        out += hex[b[i] & 0xf];
    }
    return out;
}

static bool bloom_probe_cpu(const starminer::utxo::UTXOBloomBuilder& bf,
                             const uint8_t h160[20])
{
    const auto& bits = bf.data();
    uint32_t h1 = 0, h2 = 0;
    // MurmurHash3 32-bit (matches h160_bloom_filter.cu seed=42)
    auto murmur3 = [](const uint8_t* data, uint32_t seed) -> uint32_t {
        const uint32_t c1 = 0xcc9e2d51u, c2 = 0x1b873593u;
        uint32_t h = seed;
        for (int i = 0; i < 5; ++i) {
            uint32_t k = (uint32_t)data[i*4]
                       | ((uint32_t)data[i*4+1] << 8)
                       | ((uint32_t)data[i*4+2] << 16)
                       | ((uint32_t)data[i*4+3] << 24);
            k *= c1; k = (k << 15) | (k >> 17); k *= c2;
            h ^= k; h = (h << 13) | (h >> 19); h = h * 5u + 0xe6546b64u;
        }
        h ^= 20u;
        h ^= h >> 16; h *= 0x85ebca6bu;
        h ^= h >> 13; h *= 0xc2b2ae35u;
        h ^= h >> 16;
        return h;
    };
    h1 = murmur3(h160, 42u);
    h2 = murmur3(h160, 42u ^ 0x5f3759dfu);
    uint64_t run = h1, step = h2;
    uint64_t nbits = bf.num_bits();
    for (uint32_t i = 0; i < bf.num_hashes(); ++i) {
        uint64_t idx = run % nbits;
        if (!(bits[idx / 8] & (1u << (idx & 7)))) return false;
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

    const int min_bits = args.range_scan_min_bits < 1 ? 1 : args.range_scan_min_bits;
    const int max_bits = args.range_scan_max_bits > 160 ? 160 : args.range_scan_max_bits;

    std::cout << "[*] Loading bloom filter: " << args.bloom_file << "\n";
    starminer::utxo::UTXOBloomBuilder bloom(
        starminer::utxo::UTXOBloomBuilder::Config{});
    try {
        bloom = starminer::utxo::UTXOBloomBuilder::load(args.bloom_file);
    } catch (const std::exception& e) {
        std::cerr << "[!] " << e.what() << "\n";
        return 1;
    }

    boxui::top(std::cout);
    boxui::centered(std::cout, "RANGE SCAN — BLOOM FILTER");
    boxui::top(std::cout);
    boxui::kv(std::cout, "Bloom filter", args.bloom_file);
    boxui::kv(std::cout, "Addresses", std::to_string(bloom.elements_added()));
    boxui::kv(std::cout, "Bit range",
              std::to_string(min_bits) + " to " + std::to_string(max_bits));
    if (max_bits > 42)
        boxui::kv(std::cout, "WARNING",
                  "Ranges > 42 bits may take hours/days per range");
    boxui::bottom(std::cout);
    std::cout << "\n";

    std::ofstream hitlog("range_scan_hits.txt", std::ios::app);
    uint64_t total_checked = 0, total_hits = 0;
    const auto t_start = std::chrono::steady_clock::now();
    auto t_last = t_start;

    for (int bits = min_bits; bits <= max_bits && !g_shutdown; ++bits) {
        // Range: [2^(bits-1), 2^bits - 1]
        cpu::uint256_t k{};
        if (bits == 1) {
            k.d[0] = 1;
        } else {
            const int limb = (bits - 1) / 64;
            const int off  = (bits - 1) % 64;
            k.d[limb] = (uint64_t)1 << off;
        }

        cpu::uint256_t end{};
        {
            // end = 2^bits - 1
            const int limb = bits / 64;
            const int off  = bits % 64;
            if (off == 0) {
                // 2^bits - 1: all limbs up to limb-1 are 0xFFFF...
                for (int i = 0; i < limb; ++i) end.d[i] = ~uint64_t(0);
            } else {
                end.d[limb] = ((uint64_t)1 << off) - 1;
                for (int i = 0; i < limb; ++i) end.d[i] = ~uint64_t(0);
            }
        }

        uint64_t range_size = (bits <= 63) ? ((uint64_t)1 << (bits - 1)) : UINT64_MAX;
        std::cout << "[*] Scanning " << bits << "-bit range ("
                  << range_size << " keys)...\n";

        while (!g_shutdown) {
            // k > end check
            bool done = (k.d[3] > end.d[3]) ||
                        (k.d[3] == end.d[3] && k.d[2] > end.d[2]) ||
                        (k.d[3] == end.d[3] && k.d[2] == end.d[2] &&
                         k.d[1] > end.d[1]) ||
                        (k.d[3] == end.d[3] && k.d[2] == end.d[2] &&
                         k.d[1] == end.d[1] && k.d[0] > end.d[0]);
            if (done) break;

            // Encode k as big-endian bytes for compute_hash160
            uint8_t kb[32];
            for (int i = 0; i < 4; ++i) {
                uint64_t l = k.d[3 - i];
                for (int j = 0; j < 8; ++j)
                    kb[i * 8 + j] = (uint8_t)(l >> (56 - 8 * j));
            }

            auto h160arr = cpu::compute_hash160(kb);
            ++total_checked;

            if (bloom_probe_cpu(bloom, h160arr.data())) {
                ++total_hits;
                std::string priv_hex = bytes_to_hex(kb, 32);
                std::string h160_hex = bytes_to_hex(h160arr.data(), 20);
                std::cout << "\n[HIT] bits=" << bits
                          << " priv=0x" << priv_hex
                          << " h160=" << h160_hex << "\n";
                if (hitlog) {
                    hitlog << bits << "\t0x" << priv_hex << "\t" << h160_hex << "\n";
                    hitlog.flush();
                }
            }

            // k++
            uint64_t carry = 1;
            for (int i = 0; i < 4 && carry; ++i) {
                k.d[i] += carry;
                carry = (k.d[i] == 0) ? 1 : 0;
            }

            // Progress every second
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - t_last).count() >= 1.0) {
                double secs = std::chrono::duration<double>(now - t_start).count();
                double rate = total_checked / secs;
                std::cout << "\r[*] bits=" << bits
                          << " checked=" << total_checked
                          << " rate=" << bw_format_rate(rate)
                          << " hits=" << total_hits
                          << "     " << std::flush;
                t_last = now;
            }
        }
    }

    print_summary(total_checked, total_hits, t_start);
    if (total_hits > 0)
        std::cout << "[*] Hits saved to range_scan_hits.txt\n";
    return 0;
}

}  // namespace runtime
}  // namespace starminer
