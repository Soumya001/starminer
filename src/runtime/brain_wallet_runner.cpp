/**
 * brain_wallet_runner.cpp - Brain wallet scanning mode.
 *
 * Reads passphrases from a wordlist, hashes them on GPU
 * (SHA-256 -> secp256k1 -> RIPEMD-160), and checks resulting
 * addresses against a bloom filter of funded Bitcoin addresses.
 */

#include "runtime/brain_wallet_runner.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "core/types.hpp"
#include "runtime/runtime_globals.hpp"
#include "tools/utxo_bloom_builder.hpp"
#include "ui/box_render.hpp"

#if defined(STARMINER_USE_CUDA) || defined(STARMINER_USE_ROCM)
#include "gpu/brain_wallet_gpu.hpp"
#endif

namespace starminer {
namespace runtime {

static std::string format_rate(double r) {
    if (r >= 1e9)  return std::to_string((uint64_t)(r / 1e9)) + " GH/s";
    if (r >= 1e6)  return std::to_string((uint64_t)(r / 1e6)) + " MH/s";
    if (r >= 1e3)  return std::to_string((uint64_t)(r / 1e3)) + " KH/s";
    return std::to_string((uint64_t)r) + " H/s";
}

int run_brain_wallet_mode(const Arguments& args)
{
    // ── Validate inputs ──────────────────────────────────────────────────────
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

#if !defined(STARMINER_USE_CUDA) && !defined(STARMINER_USE_ROCM)
    std::cerr << "[!] Brain wallet mode requires a GPU (CUDA or ROCm) build.\n"
              << "    This is a CPU-only build.\n";
    return 1;
#else

    // ── Load bloom filter ────────────────────────────────────────────────────
    std::cout << "[*] Loading bloom filter: " << args.bloom_file << "\n";
    starminer::utxo::UTXOBloomBuilder bloom_filter(
        starminer::utxo::UTXOBloomBuilder::Config{});
    try {
        bloom_filter = starminer::utxo::UTXOBloomBuilder::load(args.bloom_file);
    } catch (const std::exception& e) {
        std::cerr << "[!] Failed to load bloom filter: " << e.what() << "\n";
        return 1;
    }
    const auto& bdata = bloom_filter.data();
    std::cout << "[*] Bloom filter: "
              << bloom_filter.elements_added() << " addresses, "
              << (bdata.size() / 1024 / 1024) << " MB, "
              << bloom_filter.num_hashes() << " hash functions\n";

    // ── Open wordlist ────────────────────────────────────────────────────────
    std::ifstream wl(args.wordlist_file);
    if (!wl) {
        std::cerr << "[!] Cannot open wordlist: " << args.wordlist_file << "\n";
        return 1;
    }

    // ── Init GPU pipeline ────────────────────────────────────────────────────
    gpu::MultiGPUBrainWallet::Config cfg;
    cfg.gpu_ids   = args.gpu_ids;
    cfg.batch_size = (args.batch_size > 0) ? args.batch_size : 65536;

    gpu::MultiGPUBrainWallet pipeline(cfg);
    std::cout << "[*] Initializing GPU pipeline...\n";
    if (!pipeline.init()) {
        std::cerr << "[!] GPU pipeline init failed. Check GPU and drivers.\n";
        return 1;
    }

    if (!pipeline.load_bloom_filter(bdata.data(), bdata.size(),
                                    bloom_filter.num_bits(),
                                    bloom_filter.num_hashes(), 0)) {
        std::cerr << "[!] Failed to upload bloom filter to GPU.\n";
        return 1;
    }
    std::cout << "[*] Bloom filter uploaded to GPU\n\n";

    // ── Banner ───────────────────────────────────────────────────────────────
    namespace boxui = ::starminer::ui::box;
    boxui::top(std::cout);
    boxui::centered(std::cout, "BRAIN WALLET SCANNER");
    boxui::top(std::cout);
    boxui::kv(std::cout, "Wordlist", args.wordlist_file);
    boxui::kv(std::cout, "Bloom filter", args.bloom_file);
    boxui::kv(std::cout, "Batch size", std::to_string(cfg.batch_size));
    boxui::bottom(std::cout);
    std::cout << "\n";

    // ── Open hit log ─────────────────────────────────────────────────────────
    std::ofstream hitlog("brainwallet_hits.txt", std::ios::app);

    // ── Scan loop ─────────────────────────────────────────────────────────────
    uint64_t total_checked = 0;
    uint64_t total_hits    = 0;
    const auto t_start = std::chrono::steady_clock::now();
    auto t_last_print  = t_start;

    std::vector<std::string> batch;
    batch.reserve(cfg.batch_size);
    std::string line;

    while (!g_shutdown && std::getline(wl, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if ((int)line.size() > 256) continue;

        batch.push_back(line);

        if ((int)batch.size() < cfg.batch_size) continue;

        auto result = pipeline.process_batch(batch);
        total_checked += result.processed;

        for (const auto& hit : result.hits) {
            ++total_hits;
            std::cout << "\n[HIT] Passphrase: " << hit.passphrase
                      << "  scheme=" << (int)hit.scheme_id << "\n";
            if (hitlog) {
                hitlog << hit.passphrase << "\t" << (int)hit.scheme_id << "\n";
                hitlog.flush();
            }
        }

        // Progress every second
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - t_last_print).count();
        if (elapsed >= 1.0) {
            double total_s = std::chrono::duration<double>(now - t_start).count();
            double rate = total_checked / total_s;
            std::cout << "\r[*] Checked: " << total_checked
                      << "  Rate: " << format_rate(rate)
                      << "  Hits: " << total_hits
                      << "     " << std::flush;
            t_last_print = now;
        }

        batch.clear();
    }

    // Process remaining partial batch
    if (!batch.empty() && !g_shutdown) {
        auto result = pipeline.process_batch(batch);
        total_checked += result.processed;
        for (const auto& hit : result.hits) {
            ++total_hits;
            std::cout << "\n[HIT] Passphrase: " << hit.passphrase
                      << "  scheme=" << (int)hit.scheme_id << "\n";
            if (hitlog) {
                hitlog << hit.passphrase << "\t" << (int)hit.scheme_id << "\n";
                hitlog.flush();
            }
        }
    }

    // ── Summary ───────────────────────────────────────────────────────────────
    double total_s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t_start).count();
    std::cout << "\n\n";
    boxui::top(std::cout);
    boxui::centered(std::cout, "SCAN COMPLETE");
    boxui::top(std::cout);
    boxui::kv(std::cout, "Total checked", std::to_string(total_checked));
    boxui::kv(std::cout, "Total hits", std::to_string(total_hits));
    {
        std::ostringstream s;
        s << std::fixed << std::setprecision(1) << total_s << "s  ("
          << format_rate(total_s > 0 ? total_checked / total_s : 0) << ")";
        boxui::kv(std::cout, "Time / Rate", s.str());
    }
    if (total_hits > 0)
        boxui::kv(std::cout, "Hit log", "brainwallet_hits.txt");
    boxui::bottom(std::cout);
    std::cout << "\n";

    return 0;
#endif  // GPU build
}

}  // namespace runtime
}  // namespace starminer
