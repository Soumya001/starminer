/**
 * edition.hpp - starminer Feature Edition Control
 * 
 * Controls which features are available based on build configuration.
 * Free edition supports pool mining with any pool.
 * Pro edition requires a valid license for all features.
 */

#pragma once

// ============================================================================
// Version Information
// ============================================================================
#define STARMINER_VERSION "1.0.0"
#define STARMINER_MAJOR_VERSION 1

// ============================================================================
// Edition Feature Gates
// ============================================================================

    // --- Free Edition Features ---
    // Solo is available via CLI flags (--puzzle), but default is pool mode
    #define STARMINER_HAS_SOLO           1
    #define STARMINER_HAS_BRAINWALLET    0
    #define STARMINER_HAS_BLOOM          0
    #define STARMINER_HAS_GENERATORS     0
    #define STARMINER_HAS_RULES          0
    #define STARMINER_HAS_SCRAPERS       0
    #define STARMINER_HAS_CUSTOM_POOL    0
    #define STARMINER_EDITION_NAME       "starminer"
    #define STARMINER_FREE_POOL_URL      ""

// ============================================================================
// Feature Check Macros
// ============================================================================

#define STARMINER_FEATURE_AVAILABLE(feature_name) (STARMINER_HAS_##feature_name)

// ============================================================================
// Edition Information
// ============================================================================

namespace starminer {
namespace edition {

/**
 * Check if we're running the Pro edition
 */
constexpr bool is_pro() {
    return false;
}

/**
 * Check if we're running the Free edition  
 */
constexpr bool is_free() {
    return !is_pro();
}

/**
 * Get edition name
 */
constexpr const char* name() {
    return STARMINER_EDITION_NAME;
}

/**
 * Get version string
 */
constexpr const char* version() {
    return STARMINER_VERSION;
}

/**
 * Get major version
 */
constexpr int major_version() {
    return STARMINER_MAJOR_VERSION;
}

/**
 * Check if a specific feature is available
 */
constexpr bool has_solo() { return STARMINER_HAS_SOLO; }
constexpr bool has_brainwallet() { return STARMINER_HAS_BRAINWALLET; }
constexpr bool has_bloom() { return STARMINER_HAS_BLOOM; }
constexpr bool has_generators() { return STARMINER_HAS_GENERATORS; }
constexpr bool has_rules() { return STARMINER_HAS_RULES; }
constexpr bool has_scrapers() { return STARMINER_HAS_SCRAPERS; }

} // namespace edition
} // namespace starminer