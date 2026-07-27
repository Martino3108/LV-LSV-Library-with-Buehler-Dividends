/**
 * @file buehler_mc_path_pricing.h
 * @brief Shared MC statistics on BuehlerFixingSavePath payoffs.
 */

#ifndef BUEHLER_MC_PATH_PRICING_H
#define BUEHLER_MC_PATH_PRICING_H

#include <ql/quantlib.hpp>
#include <vector>
#include <vector>

struct BuehlerMcPathPricingResult {
    QuantLib::Real value = 0.0;
    QuantLib::Real errorEstimate = QuantLib::Null<QuantLib::Real>();
};

/** @brief Mean (and stderr of the mean) across independent MC sub-bank prices. */
struct McSubbankAccumulator {
    void add(const BuehlerMcPathPricingResult& result);
    BuehlerMcPathPricingResult finish() const;

private:
    std::vector<QuantLib::Real> values_;
    std::vector<QuantLib::Real> withinSubbankStderr_;
};

/**
 * @brief Single-pass payoff statistics (Welford), same semantics as
 * @c buehlerMcStatsFromPayoffs without materializing the per-path payoff buffer
 * (pricers stream payoffs in; saves one nPaths vector per option and a memory pass).
 */
class BuehlerMcPayoffAccumulator {
public:
    void add(const QuantLib::Real payoff) {
        ++n_;
        const QuantLib::Real delta = payoff - mean_;
        mean_ += delta / static_cast<QuantLib::Real>(n_);
        m2_ += delta * (payoff - mean_);
    }
    /** @brief value = valueScale·mean; errorEstimate = valueScale·stderr (Null when n < 2). */
    BuehlerMcPathPricingResult finish(QuantLib::Real valueScale) const;

private:
    QuantLib::Size n_ = 0;
    QuantLib::Real mean_ = 0.0;
    QuantLib::Real m2_ = 0.0;
};

BuehlerMcPathPricingResult buehlerMcStatsFromPayoffs(const std::vector<QuantLib::Real>& payoffs,
                                                     QuantLib::Real valueScale);

#endif
