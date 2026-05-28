/**
 * interactive_ui.cpp - StarMiner interactive startup flow.
 */

// Prevent Windows min/max macros from breaking std::min/std::max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "ui/interactive_ui.hpp"

#include "core/puzzle_analysis.hpp"
#include "core/puzzle_config.hpp"
#include "core/yaml_config.hpp"
#include "tools/utxo_bloom_builder.hpp"
#include "ui/banner.hpp"
#include "ui/interactive.hpp"
#include "core/edition.hpp"


std::string format_number(uint64_t n);
std::string format_number_human(uint64_t n);
std::string normalize_path(const std::string& path);

namespace starminer::ui {

Arguments run_puzzle_interactive(Arguments base_args, double gpu_speed_mkeys) {
    using namespace ::starminer::ui;
    Arguments args = base_args;
    args.puzzle_mode = true;

    Interactive::display_section("Bitcoin Puzzle Challenge Mode");

    // First ask: standalone or pool?
    PuzzleModeChoice mode_choice = Interactive::display_puzzle_mode_menu();

    if (mode_choice == PuzzleModeChoice::BACK) {
        args.go_back = true;  // Return to main menu
        return args;
    }

    bool use_pool = (mode_choice == PuzzleModeChoice::JOIN_POOL);

    if (use_pool) {
        // Configure pool settings
        std::string pool_url, worker;
        if (!Interactive::prompt_pool_config(pool_url, worker, args.pool_url, args.pool_worker)) {
            args.go_back = true;
            return args;
        }
        args.pool_mode = true;
        args.pool_url = pool_url;
        args.pool_worker = worker;

        // v1.4.1: persist the BTC payout address to ~/.starminer/config.yml
        // so the next launch defaults to it (user can still override at the
        // prompt). Only writes when the file doesn't already exist; never
        // overwrites an existing operator-managed config.
        const std::string saved =
            starminer::AppConfig::save_pool_worker(worker, pool_url);
        if (!saved.empty()) {
            Interactive::info_message("Saved pool worker to " + saved
                + " (will default on next launch)");
        }

        // Pool mode doesn't need puzzle selection -- pool assigns work.
        // Pre-1.4.1 the runner waited on Enter here, which forced an extra
        // keystroke after the user had ALREADY confirmed the pool/worker
        // pair in prompt_pool_config above. Just connect.
        std::cout << "\n";
        Interactive::info_message("Pool mode: Work will be assigned by the pool server. Connecting...");
        return args;
    }

    // Standalone mode - select puzzle
    std::cout << "\n";
    int puzzle_choice = Interactive::prompt_number(
        "Enter puzzle number (1-256)", 1, 256, true);

    if (puzzle_choice == -1) {
        // Auto mode - use smart selection
        int best = ::get_best_puzzle(gpu_speed_mkeys);
        if (best > 0) {
            args.puzzle_number = best;
            const ::starminer::PuzzleInfo* puzzle = ::starminer::PuzzleDatabase::get_puzzle(best);

            std::cout << "\n";
            Interactive::info_message("Analyzing puzzles...");

            if (puzzle) {
                bool has_pubkey = !puzzle->public_key_hex.empty();

                // Calculate estimated time
                std::string est_time;
                ::PuzzleAnalysis analysis = ::analyze_puzzle(puzzle, gpu_speed_mkeys);
                if (analysis.estimated_gpu_years < 0.01) {
                    est_time = "<1 week";
                } else if (analysis.estimated_gpu_years < 0.1) {
                    est_time = std::to_string((int)(analysis.estimated_gpu_years * 52)) + " weeks";
                } else if (analysis.estimated_gpu_years < 1.0) {
                    est_time = std::to_string((int)(analysis.estimated_gpu_years * 12)) + " months";
                } else {
                    est_time = "~" + std::to_string((int)analysis.estimated_gpu_years) + " years";
                }

                std::cout << "\n";
                Interactive::info_message("Smart Selection: Puzzle #" + std::to_string(best));
                Interactive::display_puzzle_info(puzzle->number, puzzle->bits, has_pubkey,
                                                 puzzle->btc_reward, est_time);

                if (has_pubkey) {
                    args.puzzle_kangaroo = true;
                }

                std::cout << "\n";
                if (!Interactive::prompt_yes_no("Proceed with Puzzle #" + std::to_string(best) + "?", true)) {
                    std::cout << "\n";
                    Interactive::info_message("Selection cancelled. Returning to main menu...");
                    args.go_back = true;
                    return args;
                }
            }
        }
    } else {
        // Specific puzzle selected
        args.puzzle_number = puzzle_choice;
        const ::starminer::PuzzleInfo* puzzle = ::starminer::PuzzleDatabase::get_puzzle(puzzle_choice);

        if (puzzle) {
            bool has_pubkey = !puzzle->public_key_hex.empty();

            // Check if solved
            if (puzzle->solved) {
                std::cout << "\n";
                Interactive::warning_message("Puzzle #" + std::to_string(puzzle_choice) + " is already SOLVED!");
                std::cout << "    Solution: " << puzzle->solution_hex << "\n";
                std::cout << "\n";

                if (Interactive::prompt_yes_no("Continue in testing mode?", true)) {
                    Interactive::info_message("Testing mode - will verify against known solution.");
                } else {
                    args.go_back = true;
                    return args;
                }
            }

            // Calculate estimated time
            std::string est_time;
            ::PuzzleAnalysis analysis = ::analyze_puzzle(puzzle, gpu_speed_mkeys);
            if (analysis.estimated_gpu_years < 0.01) {
                est_time = "<1 week";
            } else if (analysis.estimated_gpu_years < 1.0) {
                est_time = std::to_string((int)(analysis.estimated_gpu_years * 12)) + " months";
            } else {
                est_time = "~" + std::to_string((int)analysis.estimated_gpu_years) + " years";
            }

            std::cout << "\n";
            Interactive::display_puzzle_info(puzzle->number, puzzle->bits, has_pubkey,
                                             puzzle->btc_reward, est_time);

            if (has_pubkey) {
                args.puzzle_kangaroo = true;
                std::cout << "\n";
                Interactive::info_message("Auto-enabled Kangaroo algorithm (public key available)");
            }

            std::cout << "\n";
            if (!Interactive::prompt_yes_no("Start solving Puzzle #" + std::to_string(puzzle_choice) + "?", true)) {
                args.go_back = true;
                return args;
            }
        } else {
            Interactive::error_message("Unknown puzzle number: " + std::to_string(puzzle_choice));
            args.go_back = true;
            return args;
        }
    }

    return args;
}

// ── Brain Wallet interactive setup ───────────────────────────────────────────
Arguments run_brainwallet_interactive(Arguments base_args) {
    using namespace ::starminer::ui;
    Arguments args = base_args;

    Interactive::display_section("Brain Wallet Scanner");
    std::cout << "Hashes passphrases from a wordlist and checks the resulting\n"
              << "Bitcoin addresses against a bloom filter of funded wallets.\n\n";

    // Wordlist
    std::string wordlist = Interactive::prompt_path("Wordlist file path", true);
    if (wordlist.empty()) { args.go_back = true; return args; }
    args.wordlist_file = wordlist;

    // Bloom filter
    std::string bloom = Interactive::prompt_path("Bloom filter (.blf) path", true);
    if (bloom.empty()) { args.go_back = true; return args; }
    args.bloom_file = bloom;

    if (!Interactive::prompt_yes_no("Start brain wallet scan?", true)) {
        args.go_back = true;
        return args;
    }

    args.brainwallet_mode = true;
    args.pool_mode = false;
    return args;
}

// ── Range Scan interactive setup ─────────────────────────────────────────────
Arguments run_range_scan_interactive(Arguments base_args) {
    using namespace ::starminer::ui;
    Arguments args = base_args;

    Interactive::display_section("Range Scan");
    std::cout << "Sweeps every private key in [2^(N-1), 2^N-1] for each bit width N\n"
              << "and checks the resulting address against a bloom filter.\n"
              << "Practical up to ~42 bits (CPU); larger ranges take days.\n\n";

    // Bloom filter
    std::string bloom = Interactive::prompt_path("Bloom filter (.blf) path", true);
    if (bloom.empty()) { args.go_back = true; return args; }
    args.bloom_file = bloom;

    // Min bits
    int min_bits = Interactive::prompt_number("Start bit width (min)", 1, 160, false);
    if (min_bits <= 0) min_bits = 1;
    args.range_scan_min_bits = min_bits;

    // Max bits
    int max_bits = Interactive::prompt_number("End bit width (max)", min_bits, 160, false);
    if (max_bits <= 0) max_bits = 50;
    args.range_scan_max_bits = max_bits;

    if (max_bits > 42) {
        Interactive::warning_message(
            "Ranges > 42 bits are impractically slow on CPU. Continue?");
        if (!Interactive::prompt_yes_no("", true)) {
            args.go_back = true;
            return args;
        }
    }

    if (!Interactive::prompt_yes_no("Start range scan?", true)) {
        args.go_back = true;
        return args;
    }

    args.range_scan = true;
    args.brainwallet_mode = true;
    args.pool_mode = false;
    return args;
}

Arguments run_interactive_mode(Arguments base_args, double gpu_speed_mkeys) {
    using namespace ::starminer::ui;
    Arguments args = base_args;

    while (true) {
        args.go_back = false;
        MainMenuChoice choice = Interactive::display_main_menu(STARMINER_VERSION);

        switch (choice) {
            case MainMenuChoice::PUZZLE_MODE: {
                args = run_puzzle_interactive(args, gpu_speed_mkeys);
                if (args.go_back) continue;
                return args;
            }

            case MainMenuChoice::BRAINWALLET_MODE: {
                args = run_brainwallet_interactive(args);
                if (args.go_back) continue;
                return args;
            }

            case MainMenuChoice::RANGE_SCAN_MODE: {
                args = run_range_scan_interactive(args);
                if (args.go_back) continue;
                return args;
            }

            case MainMenuChoice::BENCHMARK_MODE:
                args.benchmark = true;
                std::cout << "\n";
                Interactive::info_message("Starting GPU performance benchmark...");
                return args;

            case MainMenuChoice::SHOW_HELP:
                args.help = true;
                return args;

            case MainMenuChoice::EXIT:
                args.exit_program = true;
                std::cout << "\n";
                Interactive::info_message("Goodbye!");
                return args;

            default:
                args.exit_program = true;
                return args;
        }
    }
}

void enable_windows_ansi() {
#ifdef _WIN32
    // Enable virtual terminal processing for ANSI escape codes
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode)) {
            dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, dwMode);
        }
    }
    // Also enable for stderr
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    if (hErr != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hErr, &dwMode)) {
            dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hErr, dwMode);
        }
    }
#endif
}

}  // namespace starminer::ui
