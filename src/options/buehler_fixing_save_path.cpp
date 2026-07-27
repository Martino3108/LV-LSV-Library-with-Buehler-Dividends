/**
 * @file buehler_fixing_save_path.cpp
 */

#include "buehler_fixing_save_path.h"
#include <algorithm>
#include <utility>

BuehlerFixingSavePath::BuehlerFixingSavePath(std::vector<QuantLib::Date> fixingDates,
                                             const QuantLib::Size numPaths,
                                             std::vector<BuehlerBankReal> xAtFixings,
                                             std::vector<QuantLib::Real> dividendCarryAtFixing,
                                             std::vector<QuantLib::Real> slopeGAtFixing,
                                             const QuantLib::Size mcBrownianSteps)
: fixingDates_(std::move(fixingDates)),
  numPaths_(numPaths),
  xAtFixings_(std::move(xAtFixings)),
  dividendCarryAtFixing_(std::move(dividendCarryAtFixing)),
  slopeGAtFixing_(std::move(slopeGAtFixing)),
  mcBrownianSteps_(mcBrownianSteps) {
    using namespace QuantLib;
    QL_REQUIRE(numFixings() > 0, "BuehlerFixingSavePath: empty fixing schedule");
    QL_REQUIRE(numPaths_ > 0, "BuehlerFixingSavePath: paths must be non-empty");
    QL_REQUIRE(xAtFixings_.size() == numPaths_ * numFixings(),
               "BuehlerFixingSavePath: flat path buffer size mismatch");
    QL_REQUIRE(dividendCarryAtFixing_.size() == numFixings(),
               "BuehlerFixingSavePath: D schedule size mismatch");
    QL_REQUIRE(slopeGAtFixing_.size() == numFixings(),
               "BuehlerFixingSavePath: G schedule size mismatch");
    QL_REQUIRE(mcBrownianSteps_ >= numFixings(),
               "BuehlerFixingSavePath: MC Brownian steps must be >= number of fixings");
}

QuantLib::Size BuehlerFixingSavePath::fixingIndex(const QuantLib::Date& date) const {
    using namespace QuantLib;
    const auto it = std::lower_bound(fixingDates_.begin(), fixingDates_.end(), date);
    QL_REQUIRE(it != fixingDates_.end() && *it == date,
               "BuehlerFixingSavePath: date " << date << " is not on the simulated bank");
    return static_cast<Size>(it - fixingDates_.begin());
}

bool BuehlerFixingSavePath::hasFixingDate(const QuantLib::Date& date) const {
    const auto it = std::lower_bound(fixingDates_.begin(), fixingDates_.end(), date);
    return it != fixingDates_.end() && *it == date;
}

QuantLib::Real BuehlerFixingSavePath::level(const QuantLib::Size pathIndex,
                                            const QuantLib::Size fixingIndex) const {
    return xLevel(pathIndex, fixingIndex);
}

QuantLib::Real BuehlerFixingSavePath::level(const QuantLib::Size pathIndex,
                                            const QuantLib::Date& date) const {
    return level(pathIndex, fixingIndex(date));
}
