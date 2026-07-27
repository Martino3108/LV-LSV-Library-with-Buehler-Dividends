/**
 * @file digital_mc_buehler_option.cpp
 */

#include "digital_mc_buehler_option.h"
#include "buehler_model.h"
#include "buehler_pure_x_strike.h"
#include <string>
#include <vector>

namespace {

using namespace QuantLib;

/** X quote space means the contract is written on X: the strike is already pure-X
 *  (same convention as the European pricers); S quote space maps K(S) -> kx. */
Real pureXStrikeForMc(const BuehlerModel& buehler,
                      const OptionContractParams& params,
                      const BuehlerOptionPriceSpace quoteSpace) {
    if (quoteSpace == BuehlerOptionPriceSpace::X)
        return params.strike;
    return buehlerPureXStrikeFromSpot(buehler, params.expiry, params.strike);
}

Real quoteScaleCashDigitalToS(const BuehlerModel& buehler, const Date& expiry) {
    return buehler.riskFreeTs()->discount(expiry);
}

Real digitalPayoffInX(const Real x,
                      const Real kx,
                      const bool isCall,
                      const bool assetOrNothing) {
    const bool inMoney = isCall ? (x > kx) : (x < kx);
    if (!inMoney)
        return 0.0;
    return assetOrNothing ? x : 1.0;
}

} // namespace

std::string DigitalMcBuehlerOption::scenarioExportBaseName() const {
    std::string s = std::string("training_set_digital_mc_") + (params_.isCall ? "call" : "put") + "_";
    s += (quoteSpace_ == BuehlerOptionPriceSpace::X ? "X" : "S");
    s += "_";
    s += (assetOrNothing_ ? "asset" : "cash");
    return s;
}

DigitalMcBuehlerOption::DigitalMcBuehlerOption(OptionContractParams params,
                                               const BuehlerOptionPriceSpace quoteSpace,
                                               const bool assetOrNothing)
: Option(std::move(params)), quoteSpace_(quoteSpace), assetOrNothing_(assetOrNothing) {}

BuehlerMcPathPricingResult DigitalMcBuehlerOption::priceFromSavePath(
    const BuehlerFixingSavePath& savePath,
    const OptionContractParams& params,
    const BuehlerModel& buehler,
    const BuehlerOptionPriceSpace quoteSpace,
    const bool assetOrNothing) {
    using namespace QuantLib;

    QL_REQUIRE(params.expiry > buehler.today(), "DigitalMcBuehlerOption: expiry must be after today");
    QL_REQUIRE(params.strike != Null<Real>(), "DigitalMcBuehlerOption: strike must be set");
    QL_REQUIRE(savePath.hasFixingDate(params.expiry), "DigitalMcBuehlerOption: expiry not on save path");

    const Size fixIdx = savePath.fixingIndex(params.expiry);
    const Size nPaths = savePath.numPaths();
    QL_REQUIRE(nPaths > 0, "DigitalMcBuehlerOption: empty path bank");

    const Real strikeS = params.strike;

    if (quoteSpace == BuehlerOptionPriceSpace::S && assetOrNothing) {
        BuehlerMcPayoffAccumulator stats;
        for (Size path = 0; path < nPaths; ++path) {
            const Real s = savePath.sLevel(path, fixIdx);
            const bool inMoney =
                params.isCall ? (s > strikeS) : (s < strikeS);
            stats.add(inMoney ? s : 0.0);
        }
        return stats.finish(buehler.riskFreeTs()->discount(params.expiry));
    }

    const Real kx = pureXStrikeForMc(buehler, params, quoteSpace);
    QL_REQUIRE(kx > 0.0, "DigitalMcBuehlerOption: pure-X strike must be positive");

    BuehlerMcPayoffAccumulator stats;
    for (Size path = 0; path < nPaths; ++path) {
        const Real x = savePath.xLevel(path, fixIdx);
        stats.add(digitalPayoffInX(x, kx, params.isCall, assetOrNothing));
    }

    const Real valueScale = (quoteSpace == BuehlerOptionPriceSpace::X)
                                ? 1.0
                                : quoteScaleCashDigitalToS(buehler, params.expiry);
    return stats.finish(valueScale);
}

BuehlerMcPathPricingResult DigitalMcBuehlerOption::priceWithStdError(const BuehlerModel& buehler) const {
    QL_REQUIRE(buehler.hasFixingSavePath(), "DigitalMcBuehlerOption: call simulateFixingPaths first");
    return priceFromSavePath(buehler.fixingSavePath(), params_, buehler, quoteSpace_, assetOrNothing_);
}

QuantLib::Real DigitalMcBuehlerOption::price(const BuehlerModel& buehler) const {
    return priceWithStdError(buehler).value;
}
