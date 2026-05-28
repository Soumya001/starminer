/**
 * interactive_ui.hpp - Interactive top-level menu / setup wizard glue
 * for StarMiner.
 *
 * Extracted out of src/main.cpp during the v1.4.1 A.3 refactor. Hosts the
 * argv-less startup flow (banner + main menu + per-mode submenus) and the
 * Windows ANSI enabler. The actual mode runners (puzzle / pool / brain
 * wallet) live in src/runtime/.
 *
 * `run_interactive_mode` mutates a base Arguments and returns the user's
 * selection; main() then dispatches to the matching mode runner.
 */
#pragma once

#include "cli/cli_parser.hpp"  // Arguments

namespace starminer::ui {

/**
 * Enable ANSI escape codes on Windows console.
 * Required for colored output to work properly. No-op on non-Windows.
 */
void enable_windows_ansi();

Arguments run_puzzle_interactive(Arguments base_args, double gpu_speed_mkeys);
Arguments run_brainwallet_interactive(Arguments base_args);
Arguments run_range_scan_interactive(Arguments base_args);
Arguments run_interactive_mode(Arguments base_args, double gpu_speed_mkeys);

}  // namespace starminer::ui
