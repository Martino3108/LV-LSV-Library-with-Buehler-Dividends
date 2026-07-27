/**
 * @file test_pipeline_sanity.cpp
 * @brief CTest wrapper for the end-to-end flat-BS collapse check.
 *
 * Runs @c pipeline_sanity_check_BS_fallback on @c loadConstantMock(): with a flat
 * vol surface, LV Monte Carlo (Asian/barrier/lookback/autocall) and FD
 * (European/digital/accrual) must collapse to their Black–Scholes references.
 * The check prints the full comparison tables and returns the mean absolute
 * error across all rows (S units, spot = 100), which is gated here.
 */

#include "benchmark.h"
#include <cstdlib>
#include <iostream>

int main() {
    // Mean abs error is O(MC stderr) on the MC rows; with the default
    // 1 sub-bank x 100k paths it sits well below this gate (see test log).
    constexpr double kMeanAbsErrGate = 0.10; // 0.1% of spot=100

    try {
        const double meanAbsErr = pipeline_sanity_check_BS_fallback();
        std::cout << "\npipeline_sanity gate: mean_abs_err=" << meanAbsErr
                  << " (gate " << kMeanAbsErrGate << ")\n";
        if (!(meanAbsErr < kMeanAbsErrGate)) {
            std::cerr << "FAILED: mean_abs_err " << meanAbsErr << " >= " << kMeanAbsErrGate
                      << "\n";
            return EXIT_FAILURE;
        }
    } catch (const std::exception& ex) {
        std::cerr << "FAILED with exception: " << ex.what() << "\n";
        return EXIT_FAILURE;
    }
    std::cout << "PASSED\n";
    return EXIT_SUCCESS;
}
