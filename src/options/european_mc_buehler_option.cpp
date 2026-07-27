/**
 * @file european_mc_buehler_option.cpp
 */

#include "european_mc_buehler_option.h"
#include "buehler_model.h"
#include "buehler_pure_x_strike.h"
#include <cmath>
#include <string>
#include <vector>

namespace {

using namespace QuantLib;

Real pureXStrikeForMc(const BuehlerModel& buehler,
                      const OptionContractParams& params,
                      const BuehlerOptionPriceSpace quoteSpace) {
    if (quoteSpace == BuehlerOptionPriceSpace::X)
        return params.strike;
    return buehlerPureXStrikeFromSpot(buehler, params.expiry, params.strike);
}

Real affineScaleXToS(const BuehlerModel& buehler, const Date& expiry) {
    const Real A = buehler.forward0T(expiry) - buehler.dividendCarry0T(expiry);
    QL_REQUIRE(A > 0.0, "EuropeanMcBuehlerOption: A(T)=F(0,T)-D(T) must be positive");
    return buehler.riskFreeTs()->discount(expiry) * A;
}

} // namespace

std::string EuropeanMcBuehlerOption::scenarioExportBaseName() const {
    std::string s = std::string("training_set_european_mc_") + (params_.isCall ? "call" : "put") + "_";
    s += (quoteSpace_ == BuehlerOptionPriceSpace::X ? "X" : "S");
    return s;
}

EuropeanMcBuehlerOption::EuropeanMcBuehlerOption(OptionContractParams params,
                                                 const BuehlerOptionPriceSpace quoteSpace)
: Option(std::move(params)), quoteSpace_(quoteSpace) {}

BuehlerMcPathPricingResult EuropeanMcBuehlerOption::priceFromSavePath(
    const BuehlerFixingSavePath& savePath,
    const OptionContractParams& params,
    const BuehlerModel& buehler,
    const BuehlerOptionPriceSpace quoteSpace) {
    using namespace QuantLib;

    QL_REQUIRE(params.expiry > buehler.today(), "EuropeanMcBuehlerOption: expiry must be after today");
    QL_REQUIRE(params.strike != Null<Real>(), "EuropeanMcBuehlerOption: strike must be set");
    QL_REQUIRE(savePath.hasFixingDate(params.expiry),
               "EuropeanMcBuehlerOption: expiry not on save path");

    const Real kx = pureXStrikeForMc(buehler, params, quoteSpace);
    QL_REQUIRE(kx > 0.0, "EuropeanMcBuehlerOption: pure-X strike must be positive");

    const Size fixIdx = savePath.fixingIndex(params.expiry);
    const Size nPaths = savePath.numPaths();
    QL_REQUIRE(nPaths > 0, "EuropeanMcBuehlerOption: empty path bank");

    BuehlerMcPayoffAccumulator stats;
    for (Size path = 0; path < nPaths; ++path) {
        const Real x = savePath.xLevel(path, fixIdx);
        const Real payX =
            params.isCall ? std::max(x - kx, 0.0) : std::max(kx - x, 0.0);
        stats.add(payX);
    }

    const Real valueScale =
        (quoteSpace == BuehlerOptionPriceSpace::X) ? 1.0 : affineScaleXToS(buehler, params.expiry);
    return stats.finish(valueScale);
}

BuehlerMcPathPricingResult EuropeanMcBuehlerOption::priceWithStdError(const BuehlerModel& buehler) const {
    QL_REQUIRE(buehler.hasFixingSavePath(),
               "EuropeanMcBuehlerOption: call simulateFixingPaths first");
    return priceFromSavePath(buehler.fixingSavePath(), params_, buehler, quoteSpace_);
}

QuantLib::Real EuropeanMcBuehlerOption::price(const BuehlerModel& buehler) const {
    return priceWithStdError(buehler).value;
}
