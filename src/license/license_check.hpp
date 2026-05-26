/**
 * license_check.hpp — stub for open-source / self-hosted builds.
 *
 * All features are always enabled. The validate() call always succeeds.
 * License activation is a no-op.
 */
#pragma once
#include <string>

namespace starminer {
namespace license {

struct ValidationResult {
    bool        valid = true;
    std::string email;
    std::string error;
};

inline ValidationResult validate(const std::string& /*key*/) {
    return ValidationResult{true, "open-source", ""};
}

inline std::string cache_path() {
    return "/tmp/.starminer_license_cache";
}

}  // namespace license
}  // namespace starminer
