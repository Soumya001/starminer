/**
 * brain_wallet_runner.hpp — Brain wallet mode runtime driver (stub).
 *
 * The full implementation ships when starminer_gpu and the v2 kernel
 * orchestrator are linked. This stub satisfies the compiler on CPU-only
 * or builds where the orchestrator hasn't been compiled yet.
 */
#pragma once
#include "cli/cli_parser.hpp"

namespace starminer {
namespace runtime {

int run_brain_wallet_mode(const Arguments& args);

}  // namespace runtime
}  // namespace starminer
