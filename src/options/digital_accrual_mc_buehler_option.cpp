/**
 * @file digital_accrual_mc_buehler_option.cpp
 */

#include "digital_accrual_mc_buehler_option.h"
#include "buehler_model.h"
#include "buehler_pure_x_strike.h"
#include "mc_observation_schedule.h"
#include <string>
#include <vector>

namespace {

using namespace QuantLib;

Real cashDigitalCallInX(const Real x, const Real kx) {
    return (x > kx) ? 1.0 : 0.0;
}

} // namespace

std::string DigitalAccrualMcBuehlerOption::scenarioExportBaseName() const {
    return std::string("training_set_digital_accrual_mc_") +
           (quoteSpace_ == BuehlerOptionPriceSpace::X ? "X" : "S");
}

DigitalAccrualMcBuehlerOption::DigitalAccrualMcBuehlerOption(OptionContractParams params,
                                                               const BuehlerOptionPriceSpace quoteSpace)
: Option(std::move(params)), quoteSpace_(quoteSpace) {}

BuehlerMcPathPricingResult DigitalAccrualMcBuehlerOption::priceFromSavePath(
    const BuehlerFixingSavePath& savePath,
    const OptionContractParams& params,
    const BuehlerModel& buehler,
    const BuehlerOptionPriceSpace quoteSpace) {
    using namespace QuantLib;

    QL_REQUIRE(params.strikeLow != Null<Real>(), "DigitalAccrualMcBuehlerOption: strikeLow required");
    QL_REQUIRE(params.strikeUp != Null<Real>(), "DigitalAccrualMcBuehlerOption: strikeUp required");
    QL_REQUIRE(params.strikeLow < params.strikeUp,
               "DigitalAccrualMcBuehlerOption: strikeLow must be below strikeUp");

    const std::vector<Date> observationDates =
        resolveMcObservationDates(buehler, params, "DigitalAccrualMcBuehlerOption");

    const Size nPaths = savePath.numPaths();
    QL_REQUIRE(nPaths > 0, "DigitalAccrualMcBuehlerOption: empty path bank");

    Size nLegs = 0;
    std::vector<Real> pathAccrual(nPaths, 0.0);

    for (const Date& obsDate : observationDates) {
        QL_REQUIRE(obsDate > buehler.today(),
                   "DigitalAccrualMcBuehlerOption: observation date must be after today");
        QL_REQUIRE(savePath.hasFixingDate(obsDate),
                   "DigitalAccrualMcBuehlerOption: observation date not on save path");

        // X quote space: strikes are already pure-X (constant across observation dates);
        // S quote space: map K(S) -> kx per observation date.
        const bool strikesArePureX = (quoteSpace == BuehlerOptionPriceSpace::X);
        const Real kxLow = strikesArePureX
                               ? params.strikeLow
                               : buehlerPureXStrikeFromSpot(buehler, obsDate, params.strikeLow);
        const Real kxUp = strikesArePureX
                              ? params.strikeUp
                              : buehlerPureXStrikeFromSpot(buehler, obsDate, params.strikeUp);
        QL_REQUIRE(kxLow > 0.0 && kxUp > 0.0,
                   "DigitalAccrualMcBuehlerOption: pure-X strikes must be positive");
        QL_REQUIRE(kxLow < kxUp,
                   "DigitalAccrualMcBuehlerOption: kx(Low) must be below kx(Up) at " << obsDate);

        const Size fixIdx = savePath.fixingIndex(obsDate);
        const Real legScale =
            (quoteSpace == BuehlerOptionPriceSpace::X) ? 1.0 : buehler.riskFreeTs()->discount(obsDate);

        for (Size path = 0; path < nPaths; ++path) {
            const Real x = savePath.xLevel(path, fixIdx);
            const Real legX = cashDigitalCallInX(x, kxLow) - cashDigitalCallInX(x, kxUp);
            pathAccrual[path] += legScale * legX;
        }
        ++nLegs;
    }

    QL_REQUIRE(nLegs > 0, "DigitalAccrualMcBuehlerOption: no valid observation dates");
    const Real invN = 1.0 / static_cast<Real>(nLegs);
    BuehlerMcPayoffAccumulator stats;
    for (const Real acc : pathAccrual)
        stats.add(acc * invN);

    return stats.finish(1.0);
}

BuehlerMcPathPricingResult DigitalAccrualMcBuehlerOption::priceWithStdError(
    const BuehlerModel& buehler) const {
    QL_REQUIRE(buehler.hasFixingSavePath(),
               "DigitalAccrualMcBuehlerOption: call simulateFixingPaths first");
    return priceFromSavePath(buehler.fixingSavePath(), params_, buehler, quoteSpace_);
}

QuantLib::Real DigitalAccrualMcBuehlerOption::price(const BuehlerModel& buehler) const {
    return priceWithStdError(buehler).value;
}
