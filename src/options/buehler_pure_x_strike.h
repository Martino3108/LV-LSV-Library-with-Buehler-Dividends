/**
 * @file buehler_pure_x_strike.h
 * @brief Map spot strike K_S to pure-X strike K_X at expiry.
 */

#ifndef BUEHLER_PURE_X_STRIKE_H
#define BUEHLER_PURE_X_STRIKE_H

#include <ql/types.hpp>

namespace QuantLib {
class Date;
}

class BuehlerModel;

QuantLib::Real buehlerPureXStrikeFromSpot(const BuehlerModel& b,
                                         const QuantLib::Date& expiry,
                                         QuantLib::Real strikeS);

#endif
