/**
 * cli_parser.cpp - StarMiner CLI argument parser.
 */
#include "cli/cli_parser.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

int validate_mode_mutex(const Arguments& args, std::string& msg) {
    int active = 0;
    std::vector<std::string> chosen;
    if (args.brainwallet_mode) { active++; chosen.emplace_back("--brainwallet"); }
    if (args.pool_mode)        { active++; chosen.emplace_back("--pool <url>"); }
    // --kangaroo is a sub-mode of puzzle solving. Only count it as a top-level
    // mode claim when neither brainwallet nor pool is asserted (so combining
    // --kangaroo with either still flags the conflict via the != 1 check below).
    if (args.puzzle_kangaroo && (args.brainwallet_mode || args.pool_mode)) {
        active++;
        chosen.emplace_back("--kangaroo");
    }
    if ((args.puzzle_number > 0 || args.puzzle_all_unsolved) &&
        (args.brainwallet_mode || args.pool_mode)) {
        active++;
        if (args.puzzle_all_unsolved) chosen.emplace_back("--all-unsolved");
        else chosen.emplace_back("--puzzle " + std::to_string(args.puzzle_number));
    }

    if (active <= 1) return 0;

    msg = "[!] Conflicting search modes selected: ";
    for (size_t i = 0; i < chosen.size(); ++i) {
        msg += chosen[i];
        if (i + 1 < chosen.size()) msg += ", ";
    }
    msg += ".\n";
    msg += "    Pick exactly one of: --brainwallet, --pool <url>, --puzzle <N> [--kangaroo].\n";
    msg += "    --kangaroo by itself is allowed (it implies puzzle mode);\n";
    msg += "    --puzzle <N> + --kangaroo is the standard kangaroo-on-puzzle case.\n";
    return -1;
}

int parse_args_core(int argc, char* argv[], Arguments& args,
                    starminer::CLIFlags& cli, std::string& err_msg) {
    args = Arguments{};
    cli = starminer::CLIFlags{};

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            args.help = true;
        } else if (arg == "--verbose" || arg == "-v") {
            args.verbose = true;
            cli.verbose_set = true;
        } else if ((arg == "--gpus" || arg == "-g") && i + 1 < argc) {
            args.gpu_ids.clear();
            std::string gpus = argv[++i];
            size_t pos = 0;
            while ((pos = gpus.find(',')) != std::string::npos) {
                args.gpu_ids.push_back(std::stoi(gpus.substr(0, pos)));
                gpus.erase(0, pos + 1);
            }
            args.gpu_ids.push_back(std::stoi(gpus));
            cli.gpu_ids_set = true;
        } else if (arg == "--batch-size" && i + 1 < argc) {
            args.batch_size = std::stoull(argv[++i]);
            cli.batch_size_set = true;
        } else if (arg == "--benchmark") {
            args.benchmark = true;
        } else if (arg == "--benchmark-time" && i + 1 < argc) {
            args.benchmark_seconds = std::stoi(argv[++i]);
            cli.benchmark_seconds_set = true;
        } else if (arg == "--puzzle" || arg == "-P") {
            args.puzzle_mode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                args.puzzle_number = std::stoi(argv[++i]);
                cli.puzzle_number_set = true;
            }
        } else if (arg == "--puzzle-target" && i + 1 < argc) {
            args.puzzle_target = argv[++i];
            cli.puzzle_target_set = true;
        } else if (arg == "--puzzle-start" && i + 1 < argc) {
            args.puzzle_range_start = argv[++i];
            cli.puzzle_start_set = true;
        } else if (arg == "--puzzle-end" && i + 1 < argc) {
            args.puzzle_range_end = argv[++i];
            cli.puzzle_end_set = true;
        } else if (arg == "--pubkey" && i + 1 < argc) {
            // v1.4.1: provide a 33-byte compressed pubkey (02/03 + 32B hex)
            // when scanning a target whose pubkey isn't in
            // puzzle_history.json. See README for the canonical
            // kangaroo-able puzzle list (multiples of 5 in 71-160 plus
            // every solved puzzle below 71 already have their pubkey
            // bundled).
            args.puzzle_pubkey = argv[++i];
            cli.puzzle_pubkey_set = true;
        } else if (arg == "--sequential") {
            args.puzzle_random = false;
            cli.puzzle_random_set = true;
        } else if (arg == "--random") {
            args.puzzle_random = true;
            cli.puzzle_random_set = true;
        } else if (arg == "--puzzle-checkpoint" && i + 1 < argc) {
            args.puzzle_checkpoint = argv[++i];
            cli.puzzle_checkpoint_set = true;
        } else if (arg == "--auto-next") {
            args.puzzle_auto_next = true;
            cli.puzzle_auto_next_set = true;
        } else if (arg == "--all-unsolved") {
            args.puzzle_all_unsolved = true;
        } else if (arg == "--puzzle-min-bits" && i + 1 < argc) {
            args.puzzle_min_bits = std::stoi(argv[++i]);
            cli.puzzle_min_bits_set = true;
        } else if (arg == "--puzzle-max-bits" && i + 1 < argc) {
            args.puzzle_max_bits = std::stoi(argv[++i]);
            cli.puzzle_max_bits_set = true;
        } else if (arg == "--kangaroo") {
            args.puzzle_kangaroo = true;
            cli.puzzle_kangaroo_set = true;
        } else if (arg == "--dp-bits" && i + 1 < argc) {
            args.dp_bits = std::stoi(argv[++i]);
            cli.dp_bits_set = true;
        } else if (arg == "--bloom" && i + 1 < argc) {
            args.bloom_file = argv[++i];
            cli.bloom_file_set = true;
        } else if (arg == "--brainwallet") {
            args.brainwallet_mode = true;
            args.pool_mode = false;
            args.pool_url.clear();
            cli.brainwallet_set = true;
        } else if (arg == "--wordlist" && i + 1 < argc) {
            args.wordlist_file = argv[++i];
        } else if (arg == "--range-scan") {
            args.range_scan = true;
            args.brainwallet_mode = true;
            args.pool_mode = false;
            args.pool_url.clear();
        } else if (arg == "--min-bits" && i + 1 < argc) {
            args.range_scan_min_bits = std::stoi(argv[++i]);
        } else if (arg == "--max-bits" && i + 1 < argc) {
            args.range_scan_max_bits = std::stoi(argv[++i]);
        } else if (arg == "--brainwallet-setup") {
            args.brainwallet_setup = true;
        } else if (arg == "--puzzle-only-v2") {
            std::cerr << "[!] --puzzle-only-v2 is not available in this build.\n";
            return 2;
        } else if (arg == "--puzzle-keys" && i + 1 < argc) {
            args.puzzle_keys_file = argv[++i];
        } else if (arg == "--schemes" && i + 1 < argc) {
            args.schemes_csv = argv[++i];
        } else if (arg == "--addr-types" && i + 1 < argc) {
            args.addr_types_csv = argv[++i];
        } else if (arg == "--resume") {
            args.resume = true;
            cli.resume_set = true;
        } else if (arg == "--cpu-rules") {
            args.cpu_rules = true;
        } else if (arg == "--save-interval" && i + 1 < argc) {
            args.save_interval = std::stoull(argv[++i]);
            cli.save_interval_set = true;
        } else if (arg == "--calibrate") {
            args.calibrate = true;
        } else if (arg == "--force-calibrate") {
            args.calibrate = true;
            args.force_calibrate = true;
            cli.force_calibrate_set = true;
        } else if (arg == "--debug") {
            args.debug = true;
            cli.debug_set = true;
        } else if (arg == "--analyze") {
            args.analyze_puzzles = true;
        } else if (arg == "--no-smart") {
            args.smart_select = false;
            cli.smart_select_set = true;
        } else if ((arg == "--pool" || arg == "-p") && i + 1 < argc) {
            args.pool_mode = true;
            args.pool_url = argv[++i];
            cli.pool_url_set = true;
        } else if ((arg == "--worker" || arg == "-w") && i + 1 < argc) {
            args.pool_worker = argv[++i];
            cli.pool_worker_set = true;
        } else if (arg == "--pool-password" && i + 1 < argc) {
            args.pool_password = argv[++i];
            cli.pool_password_set = true;
        } else if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            args.config_file = argv[++i];
        } else if (arg == "--update") {
            args.check_update = true;
        } else if (arg == "--no-update-check") {
            args.no_update_check = true;
        }
    }

    return validate_mode_mutex(args, err_msg);
}

Arguments parse_args(int argc, char* argv[], starminer::CLIFlags* cli_out) {
    Arguments args;
    starminer::CLIFlags cli;
    std::string err;
    if (parse_args_core(argc, argv, args, cli, err) != 0) {
        std::cerr << err;
        std::cerr << "    Run `starminer --help` for usage details.\n";
        std::exit(1);
    }
    if (cli_out) *cli_out = cli;
    return args;
}

int parse_args_for_test(int argc, char* argv[], Arguments& args,
                        starminer::CLIFlags& cli, std::string* err_out) {
    std::string err;
    int rc = parse_args_core(argc, argv, args, cli, err);
    if (err_out) *err_out = err;
    return rc;
}

void print_usage() {
    std::cout << R"(
StarMiner - Bitcoin Puzzle Pool Solver
===================================

GPU-accelerated solver for the Bitcoin Puzzle Challenge.

Usage:
  starminer [options]
  starminer --puzzle <number>

Puzzle Options:
  --puzzle, -P [N]            Target puzzle number N (default: auto-select easiest
                              unsolved). Auto-selects Kangaroo if pubkey is known
                              for that puzzle, else falls back to brute force.
  --kangaroo                  Force Pollard's Kangaroo algorithm. Only works on
                              puzzles with a revealed public key (see README).
  --dp-bits <N>               Distinguished point bits for Kangaroo (default:
                              auto-calculate). Manual override: 16-28; higher =
                              fewer DPs, lower = more DPs.
  --puzzle-target <addr>      Override target Bitcoin address.
  --puzzle-start <hex>        Override range start (hex, with 0x prefix).
  --puzzle-end <hex>          Override range end (hex, with 0x prefix).
  --pubkey <hex>              33-byte compressed public key (02/03 + 32B hex)
                              for the target. Only needed when scanning a
                              target whose pubkey isn't in puzzle_history.json
                              (rare; see README). The bundled file already
                              has every revealed pubkey for the canonical
                              Bitcoin Puzzle Challenge.
  --puzzle-checkpoint <file>  Checkpoint file for resume across runs.
  --puzzle-min-bits <N>       Lower bit-bound when iterating with --all-unsolved.
  --puzzle-max-bits <N>       Upper bit-bound when iterating with --all-unsolved.
  --sequential                Walk the search range from start to end.
  --random                    Random search within the range (default).
  --all-unsolved              Auto-progress through all unsolved puzzles.
  --auto-next                 After solving the current puzzle, advance to the
                              next one automatically.

Brain Wallet Mode:
  --brainwallet               Scan passphrases against funded Bitcoin addresses.
  --wordlist <file>           Passphrase list to scan (one per line).
  --bloom <file.blf>          Bloom filter of funded addresses (.blf format).
                              Build one with: ./build_bloom -i utxo.csv -o out.blf
  --range-scan                GPU sweep of raw key ranges against bloom filter.
                              Scans private keys k = 2^(N-1)..2^N-1 for each bit
                              width N from --min-bits to --max-bits. Requires --bloom.
  --min-bits <N>              Start bit width for range scan (default: 1).
  --max-bits <N>              End bit width for range scan (default: 50).


GPU Options:
  --gpus, -g <ids>            GPU device IDs to use (default: auto-detect all).
  --batch-size <n>            Keys per batch (default: 4000000).

Output Options:
  --verbose, -v               Verbose output.

Benchmark Mode:
  --benchmark                 Run GPU performance benchmark.
  --benchmark-time <sec>      Benchmark duration in seconds (default: 30).

Calibration:
  --calibrate                 Run GPU batch size calibration (auto on first run).
  --force-calibrate           Force re-calibration even if already saved.

Smart Selection:
  --analyze                   Show puzzle analysis with ROI ranking (no search).
  --no-smart                  Disable ROI-based puzzle auto-selection; pick the
                              lowest-numbered unsolved puzzle first when multiple
                              options exist. Has no effect when --puzzle <N> is
                              set explicitly. Distinct from --sequential, which
                              controls within-range search direction.

)";
    std::cout << R"(Pool Mode (Distributed Solving):
  --pool, -p <url>        Connect to pool for distributed Kangaroo solving
                          URL format: jlps://host:port (TLS, recommended)
                                      jlp://host:port  (no TLS, LAN only)
  --worker, -w <address>  Worker name (your Bitcoin address for rewards)
  --pool-password <pass>  Pool password (if required)

Other:
  --help, -h              Show this help message
  --debug                 Show debug output for troubleshooting
  --config, -c <file>     Use custom config file (default: ./config.yml)
  --update                Check for a new release and print upgrade instructions
  --no-update-check       Skip the background version check at startup

Configuration:
  Settings can be stored in config.yml (current directory or ~/.starminer/)
  Command-line arguments override config file settings.

Examples:
  # Auto-select easiest unsolved puzzle
  starminer

  # Target specific puzzle
  starminer --puzzle 71

  # Target puzzle with Kangaroo algorithm (if pubkey known)
  starminer --puzzle 135 --kangaroo

  # Kangaroo with manual dp_bits override (for tuning)
  starminer --puzzle 135 --kangaroo --dp-bits 25

  # Use random search pattern
  starminer --puzzle 71 --random

  # Auto-progress through all unsolved puzzles
  starminer --all-unsolved

  # Use specific GPUs
  starminer --puzzle 71 --gpus 0,1

  # Run GPU benchmark
  starminer --benchmark

)";
    std::cout << R"(  # Custom address + range, brute force (no kangaroo because no
  # revealed pubkey for an arbitrary address; see README on which
  # puzzles ARE kangaroo-able).
  starminer --puzzle-target 13zb1hQbWVsc2S7ZTZnP2G4undNNpdh5so \
           --puzzle-start 0x20000000000000000 \
           --puzzle-end   0x3ffffffffffffffff

  # Connect to pool for distributed solving
  starminer --pool jlps://your-pool-host:17403 --worker 1YourBitcoinAddress...

Performance: see the GitHub release notes for benchmarked numbers per
GPU architecture. The figures depend strongly on driver version, batch
size, and the specific kernel path; anything quoted here would go
stale within weeks of the next driver release.

)";
}
