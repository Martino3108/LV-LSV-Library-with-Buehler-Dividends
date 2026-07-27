/**
 * @file autocall_mc_buehler_option.cpp
 */

#include "autocall_mc_buehler_option.h"
#include "buehler_model.h"
#include "mc_observation_schedule.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

namespace {

using namespace QuantLib;

struct AutocallMcPricingSetup {
    std::vector<Size> obsIdx;
    std::vector<Real> legDiscount;
    Real couponAmount = 0.0;
    Real barrierLevel = 0.0;
    Real referenceNotional = 0.0;
    AutocallCouponStyle couponStyle = AutocallCouponStyle::Phoenix;
};

Real resolveReferenceNotional(const AutocallMcTerms& terms, const BuehlerModel& buehler) {
    if (terms.referenceNotional != Null<Real>())
        return terms.referenceNotional;
    return buehler.mapXtoS(buehler.today(), 1.0);
}

AutocallMcPricingSetup buildAutocallMcPricingSetup(const BuehlerFixingSavePath& bank,
                                                   const OptionContractParams& params,
                                                   const AutocallMcTerms& terms,
                                                   const BuehlerModel& buehler) {
    QL_REQUIRE(terms.couponRatePerPeriod != Null<Real>(),
               "AutocallMcBuehlerOption: couponRatePerPeriod required");
    QL_REQUIRE(terms.barrierFractionOfSpot != Null<Real>(),
               "AutocallMcBuehlerOption: barrierFractionOfSpot required");
    QL_REQUIRE(terms.couponRatePerPeriod >= 0.0,
               "AutocallMcBuehlerOption: couponRatePerPeriod must be non-negative");
    QL_REQUIRE(terms.barrierFractionOfSpot > 0.0,
               "AutocallMcBuehlerOption: barrierFractionOfSpot must be positive");

    const std::vector<Date> fixingDates =
        resolveMcObservationDates(buehler, params, "AutocallMcBuehlerOption");
    QL_REQUIRE(!fixingDates.empty(), "AutocallMcBuehlerOption: empty observation schedule");
    AutocallMcPricingSetup setup;
    setup.obsIdx.reserve(fixingDates.size());
    setup.legDiscount.reserve(fixingDates.size());
    for (const Date& d : fixingDates) {
        QL_REQUIRE(d > buehler.today(), "AutocallMcBuehlerOption: observation must be after today");
        QL_REQUIRE(bank.hasFixingDate(d),
                   "AutocallMcBuehlerOption: observation " << d << " not on save path");
        setup.obsIdx.push_back(bank.fixingIndex(d));
        setup.legDiscount.push_back(buehler.riskFreeTs()->discount(d));
    }
    QL_REQUIRE(!setup.obsIdx.empty(), "AutocallMcBuehlerOption: no observation dates");

    const Real referenceNotional = resolveReferenceNotional(terms, buehler);
    QL_REQUIRE(referenceNotional > 0.0,
               "AutocallMcBuehlerOption: reference notional must be positive");
    setup.couponAmount = terms.couponRatePerPeriod * referenceNotional;
    setup.barrierLevel = terms.barrierFractionOfSpot * referenceNotional;
    setup.referenceNotional = referenceNotional;
    setup.couponStyle = parseAutocallCouponStyle(terms.couponStyle);
    return setup;
}

Real autocallPhoenixPathPv(const BuehlerFixingSavePath& bank,
                           const Size path,
                           const AutocallMcPricingSetup& setup) {
    Real pv = 0.0;
    const Size nObs = setup.obsIdx.size();
    for (Size leg = 0; leg < nObs; ++leg) {
        const Real spot = bank.sLevel(path, setup.obsIdx[leg]);
        pv += setup.legDiscount[leg] * setup.couponAmount;
        const bool isLastLeg = leg + 1 == nObs;
        if (spot >= setup.barrierLevel || isLastLeg) {
            pv += setup.legDiscount[leg] * setup.referenceNotional;
            break;
        }
    }
    return pv;
}

Real autocallAthenaPathPv(const BuehlerFixingSavePath& bank,
                          const Size path,
                          const AutocallMcPricingSetup& setup) {
    Real accruedCoupons = 0.0;
    const Size nObs = setup.obsIdx.size();
    for (Size leg = 0; leg < nObs; ++leg) {
        accruedCoupons += setup.couponAmount;
        const Real spot = bank.sLevel(path, setup.obsIdx[leg]);
        const bool isLastLeg = leg + 1 == nObs;
        if (spot >= setup.barrierLevel || isLastLeg)
            return setup.legDiscount[leg] * (accruedCoupons + setup.referenceNotional);
    }
    return 0.0;
}

Real autocallKnockOutPathPv(const BuehlerFixingSavePath& bank,
                            const Size path,
                            const AutocallMcPricingSetup& setup) {
    switch (setup.couponStyle) {
    case AutocallCouponStyle::Phoenix:
        return autocallPhoenixPathPv(bank, path, setup);
    case AutocallCouponStyle::Athena:
        return autocallAthenaPathPv(bank, path, setup);
    default:
        QL_FAIL("AutocallMcBuehlerOption: unknown coupon style");
    }
}

} // namespace

AutocallCouponStyle parseAutocallCouponStyle(const std::string& couponStyle) {
    std::string normalized = couponStyle;
    for (char& ch : normalized)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (normalized == "phoenix")
        return AutocallCouponStyle::Phoenix;
    if (normalized == "athena")
        return AutocallCouponStyle::Athena;
    QL_FAIL("AutocallMcBuehlerOption: couponStyle must be \"phoenix\" or \"athena\", got \""
            << couponStyle << '"');
}

const char* autocallCouponStyleLabel(const AutocallCouponStyle style) {
    switch (style) {
    case AutocallCouponStyle::Phoenix:
        return "phoenix";
    case AutocallCouponStyle::Athena:
        return "athena";
    default:
        QL_FAIL("AutocallMcBuehlerOption: unknown coupon style");
    }
}

std::string AutocallMcBuehlerOption::scenarioExportBaseName() const {
    return std::string("autocall_") + autocallCouponStyleLabel(parseAutocallCouponStyle(terms_.couponStyle))
           + "_mc_S";
}

AutocallMcBuehlerOption::AutocallMcBuehlerOption(OptionContractParams params, AutocallMcTerms terms)
: Option(std::move(params)), terms_(terms) {}

BuehlerMcPathPricingResult AutocallMcBuehlerOption::priceFromSavePath(
    const BuehlerFixingSavePath& savePath,
    const OptionContractParams& params,
    const AutocallMcTerms& terms,
    const BuehlerModel& buehler) {
    const AutocallMcPricingSetup setup =
        buildAutocallMcPricingSetup(savePath, params, terms, buehler);
    const Size nPaths = savePath.numPaths();
    QL_REQUIRE(nPaths > 0, "AutocallMcBuehlerOption: empty path bank");

    BuehlerMcPayoffAccumulator stats;
    for (Size path = 0; path < nPaths; ++path)
        stats.add(autocallKnockOutPathPv(savePath, path, setup));

    return stats.finish(1.0);
}

BuehlerMcPathPricingResult AutocallMcBuehlerOption::priceWithStdError(
    const BuehlerModel& buehler) const {
    QL_REQUIRE(buehler.hasFixingSavePath(),
               "AutocallMcBuehlerOption: call simulateFixingPaths first");
    return priceFromSavePath(buehler.fixingSavePath(), params_, terms_, buehler);
}

QuantLib::Real AutocallMcBuehlerOption::price(const BuehlerModel& buehler) const {
    return priceWithStdError(buehler).value;
}
