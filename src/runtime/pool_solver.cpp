/**
 * pool_solver.cpp - Implementation of theCollider's pool-mode runtime
 * driver.
 *
 * Extracted verbatim from src/main.cpp during the v1.4.1 A.3 refactor;
 * no behavior changes.
 */
#include "runtime/pool_solver.hpp"

#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <string>

#include "core/kangaroo_backend.hpp"
#include "pool/pool_manager.hpp"
#include "ui/banner.hpp"        // ProfessionalUI
#include "ui/box_render.hpp"    // single-source-of-truth boxed UI
#include "ui/pool_progress.hpp"

#include "platform/nvml_wrapper.hpp"
#include <chrono>
#include <map>
#include <fstream>
#include <filesystem>

namespace collider::runtime {

namespace fs = std::filesystem;

// v1.5: checkpoint helpers for pool mode
static fs::path get_checkpoint_path() {
    const char* home = std::getenv("HOME");
    if (!home) home = ".";
    fs::path dir = fs::path(home) / ".starminer";
    fs::create_directories(dir);
    return dir / "checkpoint.json";
}

static bool save_checkpoint(const ::collider::pool::WorkAssignment& work) {
    try {
        fs::path path = get_checkpoint_path();
        std::ofstream f(path);
        if (!f) return false;
        f << "{\n";
        f << "  \"work_id\": " << work.work_id << ",\n";
        f << "  \"dp_bits\": " << work.dp_bits << ",\n";
        f << "  \"puzzle_name\": \"" << work.puzzle_name << "\",\n";
        f << "  \"public_key\": \"";
        for (int i = 0; i < 33; ++i) f << std::hex << std::setw(2) << std::setfill('0') << (int)work.public_key[i];
        f << "\",\n";
        f << "  \"range_start\": \"";
        for (int i = 0; i < 32; ++i) f << std::hex << std::setw(2) << std::setfill('0') << (int)work.range_start[i];
        f << "\",\n";
        f << "  \"range_end\": \"";
        for (int i = 0; i < 32; ++i) f << std::hex << std::setw(2) << std::setfill('0') << (int)work.range_end[i];
        f << "\",\n";
        f << "  \"timestamp\": " << std::dec << std::time(nullptr) << "\n";
        f << "}\n";
        return true;
    } catch (...) {
        return false;
    }
}

static bool load_checkpoint(::collider::pool::WorkAssignment& work) {
    try {
        fs::path path = get_checkpoint_path();
        if (!fs::exists(path)) return false;

        auto mtime = fs::last_write_time(path);
        auto now = fs::file_time_type::clock::now();
        auto age = std::chrono::duration_cast<std::chrono::hours>(now - mtime).count();
        if (age > 24) return false;  // Too old

        std::ifstream f(path);
        if (!f) return false;

        std::string line, pk_hex, rs_hex, re_hex;
        uint64_t work_id = 0;
        uint32_t dp_bits = 0;
        std::time_t timestamp = 0;

        while (std::getline(f, line)) {
            auto pos = line.find('"');
            if (pos == std::string::npos) continue;
            auto key = line.substr(pos + 1, line.find('"', pos + 1) - pos - 1);
            auto val_pos = line.find(':', pos);
            if (val_pos == std::string::npos) continue;
            std::string val = line.substr(val_pos + 1);
            // trim whitespace and commas/quotes
            size_t start = val.find_first_not_of(" \t\r\n,\"");
            size_t end = val.find_last_not_of(" \t\r\n,\"");
            if (start == std::string::npos) continue;
            val = val.substr(start, end - start + 1);

            if (key == "work_id") work_id = std::stoull(val);
            else if (key == "dp_bits") dp_bits = std::stoul(val);
            else if (key == "puzzle_name") work.puzzle_name = val;
            else if (key == "public_key") pk_hex = val;
            else if (key == "range_start") rs_hex = val;
            else if (key == "range_end") re_hex = val;
            else if (key == "timestamp") timestamp = std::stoll(val);
        }

        if (pk_hex.size() != 66 || rs_hex.size() != 64 || re_hex.size() != 64) return false;

        work.work_id = work_id;
        work.dp_bits = dp_bits;
        for (int i = 0; i < 33; ++i) {
            work.public_key[i] = std::stoi(pk_hex.substr(i * 2, 2), nullptr, 16);
        }
        for (int i = 0; i < 32; ++i) {
            work.range_start[i] = std::stoi(rs_hex.substr(i * 2, 2), nullptr, 16);
            work.range_end[i] = std::stoi(re_hex.substr(i * 2, 2), nullptr, 16);
        }
        return true;
    } catch (...) {
        return false;
    }
}

// Pool-mining entry point. The local PoolManager's destructor disconnects
// the TLS/TCP session automatically, so error paths can `return 1;`
// without manual cleanup; the explicit disconnect at end-of-flow exists
// only so the final "Disconnected from pool" message lands deterministically
// before the session summary.
int run_pool_mode(const Arguments& args, const GPUDetectionResult& gpu_info) {
    using namespace ::collider::pool;

    {
        namespace boxui = ::collider::ui::box;
        std::cout << "\n";
        boxui::top(std::cout);
        boxui::centered(std::cout, "STAR POOL MODE - Distributed Kangaroo Solving");
        boxui::bottom(std::cout);
        std::cout << "\n";
    }

    // Validate arguments
    if (args.pool_url.empty()) {
        std::cerr << "[!] Error: Pool URL required (--pool <url>)\n";
        return 1;
    }

    if (args.pool_worker.empty()) {
        std::cerr << "[!] Error: Worker name required (--worker <bitcoin_address>)\n";
        std::cerr << "    Your Bitcoin address is used for reward distribution.\n";
        return 1;
    }

    // Parse pool URL
    PoolConfig pool_config;
    if (!parse_pool_url(args.pool_url, pool_config)) {
        std::cerr << "[!] Error: Invalid pool URL format\n";
        std::cerr << "    Expected: jlps://host:port (TLS) or jlp://host:port\n";
        return 1;
    }

    pool_config.worker_name = args.pool_worker;
    pool_config.password = args.pool_password;
    pool_config.api_key = args.pool_api_key;
    pool_config.debug_mode = args.debug;

    std::cout << "[*] Pool Configuration:\n";
    std::cout << "    Type:   " << pool_config.type << "\n";
    std::cout << "    Host:   " << pool_config.host << ":" << pool_config.port << "\n";
    std::cout << "    Worker: " << pool_config.worker_name << "\n";
    std::cout << "    GPUs:   " << gpu_info.device_count << " detected\n\n";

    // Connect to pool
    std::cout << "[*] Connecting to pool...\n";
    auto& pool_manager = get_pool_manager();
    pool_manager.set_config(pool_config);

    if (!pool_manager.connect()) {
        std::cerr << "[!] Failed to connect to pool\n";
        return 1;
    }

    std::cout << "[+] Connected to pool successfully!\n\n";

    // Get work assignment
    WorkAssignment work;
    if (load_checkpoint(work)) {
        std::cout << "[*] Resumed from checkpoint (work_id=" << work.work_id << ")\n";
        std::cout << "[*] Requesting work from pool...\n";
        // Still request work to validate/reauth, but use checkpoint if server gives same chunk
        WorkAssignment server_work;
        if (pool_manager.get_work(server_work)) {
            if (server_work.work_id == work.work_id) {
                std::cout << "[+] Server confirmed checkpoint work_id=" << work.work_id << "\n";
            } else {
                std::cout << "[*] Server assigned new work_id=" << server_work.work_id
                          << " (discarding checkpoint work_id=" << work.work_id << ")\n";
                work = server_work;
            }
        }
    } else {
        std::cout << "[*] Requesting work from pool...\n";
        if (!pool_manager.get_work(work)) {
            std::cerr << "[!] Failed to get work assignment from pool\n";
            return 1;  // PoolManager dtor disconnects automatically.
        }
    }

    std::cout << "[+] Work assigned: " << work.puzzle_name << "\n";
    std::cout << "    DP Bits: " << work.dp_bits << "\n";
    std::cout << "    Work ID: " << work.work_id << "\n\n";    

    // Set solution callback
    pool_manager.set_solution_callback([](const uint8_t* key, const std::string& worker) {
        namespace boxui = ::collider::ui::box;
        std::cout << "\n";
        boxui::top(std::cout);
        boxui::centered(std::cout, "SOLUTION FOUND!", boxui::ansi::BRIGHT_GREEN);
        boxui::top(std::cout);
        boxui::kv(std::cout, "Worker", worker);
        boxui::bottom(std::cout);
        // Full 64-char hex key prints below the box; the kv() budget is
        // narrower than the key length, so emitting it outside the border
        // preserves every byte without "..." truncation.
        std::cout << "  Key: ";
        for (int i = 0; i < 32; i++) {
            std::printf("%02x", key[i]);
        }
        std::cout << "\n";
    });

    // ---- Backend dispatch ----------------------------------------------
    // The three pool branches that used to live here (CUDA RCKangaroo /
    // Apple Metal Kangaroo / CPU KangarooSolver) collapsed behind the
    // IKangarooBackend interface. The factory picks the right one for
    // this build configuration; everything past this point is
    // backend-agnostic.
    auto backend = ::collider::kangaroo::create_kangaroo_backend(args.gpu_ids);

    std::cout << "[*] Initializing " << backend->name() << " for pool solving...\n";
    if (!backend->initialize(work)) {
        std::cerr << "[!] " << backend->name()
                  << " initialization failed: " << backend->error() << "\n";
        return 1;
    }

#ifdef COLLIDER_PRO
    // Bloom filter loading is a Pro feature and is supported only by the
    // CUDA backend today. Other backends silently return false from
    // try_set_bloom_filter (interface default).
    if (!args.bloom_file.empty()) {
        if (backend->try_set_bloom_filter(args.bloom_file)) {
            std::cout << "[*] Bloom filter loaded - opportunistic address checking enabled\n";
        } else if (!backend->error().empty()) {
            std::cerr << "[!] WARNING: Failed to load bloom filter: "
                      << args.bloom_file << " (" << backend->error() << ")\n";
        }
        // Backends that simply don't support bloom filtering fall through
        // silently -- not an error; the user opted in but the platform
        // doesn't have the feature.
    }
#endif

    // Pool Solving header. Backend supplies its own name + device summary.
    std::cout << "\n";
    ::collider::ui::ProfessionalUI::render_section("Pool Solving - " + backend->name());
    ::collider::ui::ProfessionalUI::render_kv("Pool",
        pool_config.host + ":" + std::to_string(pool_config.port));
    ::collider::ui::ProfessionalUI::render_kv("Worker", pool_config.worker_name);
    ::collider::ui::ProfessionalUI::render_kv("Device", backend->device_summary());
    ::collider::ui::ProfessionalUI::render_kv("DP Bits", std::to_string(work.dp_bits));
    std::cout << "\n";
    ::collider::ui::ProfessionalUI::render_footer("Press Ctrl+C to stop");

    // Wire the backend callbacks. Pool-side baseline capture for the
    // session-share % is owned by PoolProgressDisplay; the lambdas here
    // are pure adapters between IKangarooBackend's surface and the pool
    // client / progress widget.
    ::collider::ui::PoolProgressDisplay progress;
    ::collider::kangaroo::BackendCallbacks cb;

    // v1.5: NVML thermal monitoring and telemetry
    ::collider::platform::NvmlWrapper nvml;
    bool nvml_ok = nvml.init();
    if (nvml_ok) {
        std::cout << "[*] NVML initialized - GPU thermal monitoring active\n";
    }
    auto last_telemetry_time = std::chrono::steady_clock::now();
    const auto telemetry_interval = std::chrono::seconds(5);
    auto last_checkpoint_time = std::chrono::steady_clock::now();
    const auto checkpoint_interval = std::chrono::seconds(60);

    cb.on_dp = [&pool_manager](const uint8_t* x_be, const uint8_t* d_be,
                                uint8_t type, uint32_t dp_bits) {
        pool_manager.submit_dp(x_be, d_be, type, dp_bits);
    };
    cb.on_progress = [&progress, &pool_manager, &nvml, nvml_ok, &last_telemetry_time,
                      telemetry_interval, &last_checkpoint_time, checkpoint_interval,
                      &work, &args](double ops_per_sec,
                                    uint64_t local_dps) -> bool {
        if (g_shutdown.load()) return false;
        const ::collider::pool::PoolStats ps = pool_manager.get_stats();
        progress.tick(ops_per_sec,
                      local_dps,
                      pool_manager.get_submitted_count(),
                      ps.total_dps,
                      ps.your_dps);

        // v1.5: periodic checkpoint save
        {
            auto now = std::chrono::steady_clock::now();
            if (now - last_checkpoint_time >= checkpoint_interval) {
                last_checkpoint_time = now;
                if (save_checkpoint(work)) {
                    // Silent save to avoid log spam
                }
            }
        }

        // v1.5: periodic telemetry upload
        if (nvml_ok) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_telemetry_time >= telemetry_interval) {
                last_telemetry_time = now;
                auto telemetry = nvml.get_telemetry(args.gpu_ids);
                std::map<int, std::string> gpu_names;
                std::map<int, double> gpu_mhs;
                for (const auto& gt : telemetry) {
                    gpu_names[gt.index] = gt.name;
                    // Approximate MKeys/s from overall ops_per_sec divided by GPU count
                    // or use per-GPU utilization as proxy. For now report aggregate
                    // on GPU 0, 0 on others to avoid overcounting.
                    if (gt.index == args.gpu_ids.empty() ? 0 : args.gpu_ids[0]) {
                        gpu_mhs[gt.index] = ops_per_sec / 1e6;
                    } else {
                        gpu_mhs[gt.index] = 0.0;
                    }
                }
                pool_manager.set_gpu_telemetry(gpu_names, gpu_mhs);
            }
        }
        return true;
    };
    cb.on_solution = [&pool_manager](const uint8_t key[32]) {
        std::cout << "\n[+] SOLUTION FOUND!\n    Private Key: ";
        for (int i = 0; i < 32; ++i) {
            std::printf("%02x", key[i]);
        }
        std::cout << "\n";
        pool_manager.report_solution(key);
    };
    cb.should_continue = [&pool_manager, &nvml, nvml_ok]() {
        if (g_shutdown.load()) return false;
        if (pool_manager.solution_found()) {
            std::cout << "\n[*] Another worker found the solution. Stopping gracefully.\n";
            return false;
        }
        // v1.5: thermal protection
        if (nvml_ok) {
            bool overheat = nvml.check_thermal_protection(
                75,  // warn threshold
                85,  // critical threshold
                [](const ::collider::platform::GpuTelemetry& gt) {
                    std::cerr << "[NVML] WARNING: GPU " << gt.index << " (" << gt.name
                              << ") temp " << gt.temperature_c << "C\n";
                });
            if (overheat) {
                g_shutdown.store(true);
                return false;
            }
        }
        return true;
    };

    backend->solve(cb);
    std::cout << "\n[*] Solving stopped\n";

    // Disconnect from pool
    pool_manager.disconnect();
    std::cout << "\n[*] Disconnected from pool\n";

    // Print final stats
    PoolStats stats = pool_manager.get_stats();
    std::cout << "\n[*] Session Summary:\n";
    std::cout << "    DPs Submitted: " << pool_manager.get_submitted_count() << "\n";
    std::cout << "    Your Share:    " << std::fixed << std::setprecision(4)
              << (stats.your_share * 100) << "%\n";

    return 0;
}

}  // namespace collider::runtime
