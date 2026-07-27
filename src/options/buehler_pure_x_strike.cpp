/**
 * @file buehler_pure_x_strike.cpp
 * @brief Spot-to-pure-X strike mapping for Buehler FD pricers.
 */

#include "buehler_pure_x_strike.h"
#include "buehler_model.h"
#include <ql/quantlib.hpp>

QuantLib::Real buehlerPureXStrikeFromSpot(const BuehlerModel& b,
                                         const QuantLib::Date& expiry,
                                         QuantLib::Real strikeS) {
    using namespace QuantLib;
    const Real D = b.dividendCarry0T(expiry);
    const Real A = b.forward0T(expiry) - D;
    QL_REQUIRE(A > 0.0, "buehlerPureXStrikeFromSpot: affine slope A(T) must be positive");
    QL_REQUIRE(strikeS > D, "buehlerPureXStrikeFromSpot: strike S must exceed dividend escrow D(T)");
    return (strikeS - D) / A;
}
