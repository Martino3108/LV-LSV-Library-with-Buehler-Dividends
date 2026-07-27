/**
 * @file lv_european_fd_buehler_option.cpp
 * @brief `LvEuropeanFdBuehlerOption` pricing (FD call in X; put from parity).
 */

#include "lv_european_fd_buehler_option.h"
#include "buehler_model.h"
#include "buehler_pure_x_strike.h"
#include "fd_buehler_x_fdm.h"
#include <ql/quantlib.hpp>
#include <string>

std::string LvEuropeanFdBuehlerOption::scenarioExportBaseName() const {
    std::string s = std::string("training_set_lv_european_fd_") + (params_.isCall ? "call" : "put") + "_";
    s += (quoteSpace_ == BuehlerOptionPriceSpace::X ? "X" : "S");
    return s;
}

QuantLib::Real LvEuropeanFdBuehlerOption::pureXStrikeFromSpot(const BuehlerModel& b,
                                                           const QuantLib::Date& expiry,
                                                           QuantLib::Real strikeS) {
    return buehlerPureXStrikeFromSpot(b, expiry, strikeS);
}

QuantLib::Size LvEuropeanFdBuehlerOption::effectiveFdTimeSteps(const BuehlerModel& buehler) const {
    return effectiveBuehlerFdTimeSteps(buehler, params_.expiry, tGridPerYear_);
}

LvEuropeanFdBuehlerOption::LvEuropeanFdBuehlerOption(OptionContractParams params,
                                                 QuantLib::Size tGridPerYear,
                                                 QuantLib::Size xGrid)
: Option(std::move(params)), tGridPerYear_(tGridPerYear), xGrid_(xGrid) {
    QL_REQUIRE(tGridPerYear_ > 0, "LvEuropeanFdBuehlerOption: tGridPerYear must be positive");
    QL_REQUIRE(xGrid_ > 2, "LvEuropeanFdBuehlerOption: xGrid must be > 2");
}

LvEuropeanFdBuehlerOption::LvEuropeanFdBuehlerOption(OptionContractParams params,
                                                 BuehlerOptionPriceSpace buehlerPriceSpace,
                                                 QuantLib::Size tGridPerYear,
                                                 QuantLib::Size xGrid)
: LvEuropeanFdBuehlerOption(std::move(params), tGridPerYear, xGrid) {
    quoteSpace_ = buehlerPriceSpace;
}

QuantLib::Real LvEuropeanFdBuehlerOption::pureXStrikeForPricing(const BuehlerModel& buehler) const {
    if (quoteSpace_ == BuehlerOptionPriceSpace::X)
        return params_.strike;
    return pureXStrikeFromSpot(buehler, params_.expiry, params_.strike);
}

QuantLib::Real LvEuropeanFdBuehlerOption::priceCallInX(const BuehlerModel& buehler) const {
    using namespace QuantLib;

    const Date& expiry = params_.expiry;
    const Real kx = pureXStrikeForPricing(buehler);
    QL_REQUIRE(kx > 0.0, "LvEuropeanFdBuehlerOption: pure-X strike must be positive");

    const auto process = makeBuehlerPureXLocalVolProcess(buehler);
    const ext::shared_ptr<StrikedTypePayoff> payoff =
        ext::make_shared<PlainVanillaPayoff>(QuantLib::Option::Call, kx);
    const ext::shared_ptr<Exercise> exercise = ext::make_shared<EuropeanExercise>(expiry);
    const Size tGrid = effectiveFdTimeSteps(buehler);

    return fdVanillaNPVInX(process, payoff, exercise, xGrid_, tGrid, kx);
}

QuantLib::Real LvEuropeanFdBuehlerOption::price(const BuehlerModel& buehler) const {
    using namespace QuantLib;

    QL_REQUIRE(!buehler.riskFreeTs().empty(), "LvEuropeanFdBuehlerOption: empty risk-free curve in BuehlerModel");
    QL_REQUIRE(!buehler.fixedPureLocalVolTs().empty(),
               "LvEuropeanFdBuehlerOption: empty fixed pure-X local vol (run BuehlerModel::calibration)");
    QL_REQUIRE(params_.expiry > buehler.today(), "LvEuropeanFdBuehlerOption: expiry must be after today");
    QL_REQUIRE(params_.strike != Null<Real>(), "LvEuropeanFdBuehlerOption: strike must be set");

    const Date& expiry = params_.expiry;
    const Real T = buehler.dayCounter().yearFraction(buehler.today(), expiry);
    QL_REQUIRE(T > 0.0, "LvEuropeanFdBuehlerOption: non-positive time to expiry");

    const Real kx = pureXStrikeForPricing(buehler);
    QL_REQUIRE(kx > 0.0, "LvEuropeanFdBuehlerOption: pure-X strike must be positive");

    const Real callX = priceCallInX(buehler);
    const Real npvX = params_.isCall ? callX : callX - (1.0 - kx);

    if (quoteSpace_ == BuehlerOptionPriceSpace::X) {
        return npvX;
    }
    const Real A = buehler.forward0T(expiry) - buehler.dividendCarry0T(expiry);
    QL_REQUIRE(A > 0.0,
               "LvEuropeanFdBuehlerOption: A(T)=F(0,T)-D(T) must be positive for S-dynamics mapping");
    const Real discount = buehler.riskFreeTs()->discount(expiry);
    return discount * A * npvX;
}
