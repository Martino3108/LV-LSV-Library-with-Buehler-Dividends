/**
 * @file lv_digital_fd_buehler_option.cpp
 * @brief `LvDigitalFdBuehlerOption` pricing (FD call in X; put from parity).
 */

#include "lv_digital_fd_buehler_option.h"
#include "buehler_model.h"
#include "buehler_pure_x_strike.h"
#include "fd_buehler_x_fdm.h"
#include <ql/instruments/payoffs.hpp>
#include <ql/quantlib.hpp>
#include <string>

namespace {

/** @f$\mathbb{E}[X_T]@f$ on the unit pure-X FD process (@c x0=1, flat zero rates in X). */
constexpr QuantLib::Real kPureXUnitForwardAtExpiry = 1.0;

} // namespace

std::string LvDigitalFdBuehlerOption::scenarioExportBaseName() const {
    std::string s = std::string("training_set_lv_digital_fd_") + (params_.isCall ? "call" : "put") + "_";
    s += (quoteSpace_ == BuehlerOptionPriceSpace::X ? "X" : "S");
    s += "_";
    s += (assetOrNothing_ ? "asset" : "cash");
    return s;
}

QuantLib::Real LvDigitalFdBuehlerOption::pureXStrikeFromSpot(const BuehlerModel& b,
                                                          const QuantLib::Date& expiry,
                                                          QuantLib::Real strikeS) {
    return buehlerPureXStrikeFromSpot(b, expiry, strikeS);
}

QuantLib::Size LvDigitalFdBuehlerOption::effectiveFdTimeSteps(const BuehlerModel& buehler) const {
    return effectiveBuehlerFdTimeSteps(buehler, params_.expiry, tGridPerYear_);
}

LvDigitalFdBuehlerOption::LvDigitalFdBuehlerOption(OptionContractParams params,
                                               QuantLib::Size tGridPerYear,
                                               QuantLib::Size xGrid,
                                               bool assetOrNothing)
: Option(std::move(params)),
  tGridPerYear_(tGridPerYear),
  xGrid_(xGrid),
  assetOrNothing_(assetOrNothing) {
    QL_REQUIRE(tGridPerYear_ > 0, "LvDigitalFdBuehlerOption: tGridPerYear must be positive");
    QL_REQUIRE(xGrid_ > 2, "LvDigitalFdBuehlerOption: xGrid must be > 2");
}

LvDigitalFdBuehlerOption::LvDigitalFdBuehlerOption(OptionContractParams params,
                                               BuehlerOptionPriceSpace buehlerPriceSpace,
                                               QuantLib::Size tGridPerYear,
                                               QuantLib::Size xGrid,
                                               bool assetOrNothing)
: LvDigitalFdBuehlerOption(std::move(params), tGridPerYear, xGrid, assetOrNothing) {
    quoteSpace_ = buehlerPriceSpace;
}

QuantLib::Real LvDigitalFdBuehlerOption::priceCashDigitalCallInX(const BuehlerModel& buehler,
                                                                 const QuantLib::Real kx) const {
    using namespace QuantLib;

    const auto process = makeBuehlerPureXLocalVolProcess(buehler);
    const ext::shared_ptr<StrikedTypePayoff> payoff =
        ext::make_shared<CashOrNothingPayoff>(QuantLib::Option::Call, kx, 1.0);
    const ext::shared_ptr<Exercise> exercise = ext::make_shared<EuropeanExercise>(params_.expiry);
    const Size tGrid = effectiveFdTimeSteps(buehler);
    const Size xSteps = xGrid_ * kFdDigitalXGridMultiplier;
    const Size tSteps = tGrid * kFdDigitalTimeStepMultiplier;
    return fdVanillaNPVInX(process, payoff, exercise, xSteps, tSteps, kx);
}

QuantLib::Real LvDigitalFdBuehlerOption::priceAssetDigitalCallInX(const BuehlerModel& buehler,
                                                                  const QuantLib::Real kx) const {
    using namespace QuantLib;

    const Size tGrid = effectiveFdTimeSteps(buehler);
    const Size xSteps = xGrid_ * kFdDigitalXGridMultiplier;
    const Size tSteps = tGrid * kFdDigitalTimeStepMultiplier;

    const auto process = makeBuehlerPureXLocalVolProcess(buehler);
    const ext::shared_ptr<StrikedTypePayoff> payoff =
        ext::make_shared<AssetOrNothingPayoff>(QuantLib::Option::Call, kx);
    const ext::shared_ptr<Exercise> exercise = ext::make_shared<EuropeanExercise>(params_.expiry);
    return fdVanillaNPVInX(process, payoff, exercise, xSteps, tSteps, kx);
}

QuantLib::Real LvDigitalFdBuehlerOption::price(const BuehlerModel& buehler) const {
    using namespace QuantLib;

    QL_REQUIRE(!buehler.riskFreeTs().empty(), "LvDigitalFdBuehlerOption: empty risk-free curve in BuehlerModel");
    QL_REQUIRE(!buehler.fixedPureLocalVolTs().empty(),
               "LvDigitalFdBuehlerOption: empty fixed pure-X local vol (run BuehlerModel::calibration)");
    QL_REQUIRE(params_.expiry > buehler.today(), "LvDigitalFdBuehlerOption: expiry must be after today");
    QL_REQUIRE(params_.strike != Null<Real>(), "LvDigitalFdBuehlerOption: strike must be set");

    const Date& expiry = params_.expiry;
    const Real T = buehler.dayCounter().yearFraction(buehler.today(), expiry);
    QL_REQUIRE(T > 0.0, "LvDigitalFdBuehlerOption: non-positive time to expiry");

    // X quote space means the contract is written on X: the strike is already pure-X
    // (same convention as LvEuropeanFdBuehlerOption); S quote space maps K(S) -> kx.
    const Real kx = (quoteSpace_ == BuehlerOptionPriceSpace::X)
                        ? params_.strike
                        : pureXStrikeFromSpot(buehler, expiry, params_.strike);
    QL_REQUIRE(kx > 0.0, "LvDigitalFdBuehlerOption: pure-X strike must be positive");

    const Real callCashX = priceCashDigitalCallInX(buehler, kx);
    const Real cashX =
        params_.isCall ? callCashX : (kPureXUnitForwardAtExpiry - callCashX);

    if (!assetOrNothing_) {
        if (quoteSpace_ == BuehlerOptionPriceSpace::X) {
            return cashX;
        }
        return buehler.riskFreeTs()->discount(expiry) * cashX;
    }

    const Real callAssetX = priceAssetDigitalCallInX(buehler, kx);
    const Real assetX =
        params_.isCall ? callAssetX : (kPureXUnitForwardAtExpiry - callAssetX);

    if (quoteSpace_ == BuehlerOptionPriceSpace::X) {
        return assetX;
    }

    const Real D = buehler.dividendCarry0T(expiry);
    const Real A = buehler.forward0T(expiry) - D;
    QL_REQUIRE(A > 0.0,
               "LvDigitalFdBuehlerOption: A(T)=F(0,T)-D(T) must be positive for S asset digital");
    return buehler.riskFreeTs()->discount(expiry) * (A * assetX + D * cashX);
}
