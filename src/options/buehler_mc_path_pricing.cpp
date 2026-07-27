/**
 * @file buehler_mc_path_pricing.cpp
 */

#include "buehler_mc_path_pricing.h"
#include <ql/errors.hpp>
#include <cmath>

void McSubbankAccumulator::add(const BuehlerMcPathPricingResult& result) {
    values_.push_back(result.value);
    if (result.errorEstimate != QuantLib::Null<QuantLib::Real>())
        withinSubbankStderr_.push_back(result.errorEstimate);
}

BuehlerMcPathPricingResult McSubbankAccumulator::finish() const {
    using namespace QuantLib;

    BuehlerMcPathPricingResult out;
    const Size n = values_.size();
    QL_REQUIRE(n > 0, "McSubbankAccumulator::finish: no sub-bank samples");

    Real sum = 0.0;
    for (const Real v : values_)
        sum += v;
    out.value = sum / static_cast<Real>(n);

    if (n > 1) {
        Real sumSq = 0.0;
        for (const Real v : values_) {
            const Real d = v - out.value;
            sumSq += d * d;
        }
        out.errorEstimate =
            std::sqrt(sumSq / static_cast<Real>(n - 1)) / std::sqrt(static_cast<Real>(n));
    } else if (!withinSubbankStderr_.empty()) {
        out.errorEstimate = withinSubbankStderr_.front();
    }
    return out;
}

BuehlerMcPathPricingResult BuehlerMcPayoffAccumulator::finish(
    const QuantLib::Real valueScale) const {
    using namespace QuantLib;

    BuehlerMcPathPricingResult out;
    QL_REQUIRE(n_ > 0, "BuehlerMcPayoffAccumulator::finish: empty payoff sample");
    out.value = valueScale * mean_;
    if (n_ > 1) {
        const Real variance = m2_ / static_cast<Real>(n_ - 1);
        out.errorEstimate = valueScale * std::sqrt(variance / static_cast<Real>(n_));
    }
    return out;
}

BuehlerMcPathPricingResult buehlerMcStatsFromPayoffs(const std::vector<QuantLib::Real>& payoffs,
                                                     const QuantLib::Real valueScale) {
    using namespace QuantLib;

    BuehlerMcPathPricingResult out;
    const Size n = payoffs.size();
    QL_REQUIRE(n > 0, "buehlerMcStatsFromPayoffs: empty payoff sample");

    Real sum = 0.0;
    for (const Real x : payoffs)
        sum += x;
    const Real mean = sum / static_cast<Real>(n);
    out.value = valueScale * mean;

    if (n > 1) {
        Real sumSq = 0.0;
        for (const Real x : payoffs) {
            const Real d = x - mean;
            sumSq += d * d;
        }
        const Real variance = sumSq / static_cast<Real>(n - 1);
        out.errorEstimate = valueScale * std::sqrt(variance / static_cast<Real>(n));
    }
    return out;
}
