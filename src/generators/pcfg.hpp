#pragma once
// Stub: PCFG (Probabilistic Context-Free Grammar) trainer interface.
// Full implementation is not required for pool mining mode.

#include <string>
#include <cstddef>
#include <stdexcept>

namespace pcfg {

class Grammar {
public:
    struct Stats {
        size_t num_structures    = 0;
        size_t num_non_terminals = 0;
        size_t num_terminals     = 0;
        double avg_terminals_per_nt = 0.0;
    };

    Stats get_stats() const { return {}; }
    void  save(const std::string& /*path*/) {
        throw std::runtime_error("PCFG not available in this build");
    }
};

class Trainer {
public:
    struct Config {
        int  min_length               = 4;
        int  max_length               = 64;
        bool detect_keyboard_patterns = true;
        bool detect_multiwords        = true;
        int  max_terminals_per_nt     = 100000;
    };

    struct TrainingStats {
        size_t total_passwords = 0;
    };

    explicit Trainer(const Config& /*cfg*/) {}

    void          train(const std::string& /*wordlist_path*/) {}
    TrainingStats get_training_stats() const { return {}; }
    Grammar       build_grammar() const { return {}; }
};

} // namespace pcfg
