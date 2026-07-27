/**
 * @file bs_flat_mc_save_path.cpp
 */

#include "bs_flat_mc_save_path.h"
#include "benchmark_bs_flat_reference.h"
#include "market_data.h"
#include <ql/math/randomnumbers/rngtraits.hpp>
#include <ql/methods/montecarlo/pathgenerator.hpp>
#include <ql/timegrid.hpp>
#include <algorithm>

BsFlatMcSavePath::BsFlatMcSavePath(std::vector<QuantLib::Date> fixingDates,
                                   std::vector<QuantLib::Size> pathTimeIndexAtFixing,
                                   QuantLib::TimeGrid timeGrid, std::vector<QuantLib::Path> paths)
: fixingDates_(std::move(fixingDates)),
  pathTimeIndexAtFixing_(std::move(pathTimeIndexAtFixing)),
  timeGrid_(std::move(timeGrid)),
  paths_(std::move(paths)) {
    using namespace QuantLib;
    QL_REQUIRE(!fixingDates_.empty(), "BsFlatMcSavePath: empty fixing schedule");
    QL_REQUIRE(!paths_.empty(), "BsFlatMcSavePath: paths must be non-empty");
    QL_REQUIRE(pathTimeIndexAtFixing_.size() == fixingDates_.size(),
               "BsFlatMcSavePath: path time index size mismatch");
    for (const Path& p : paths_) {
        QL_REQUIRE(p.length() == timeGrid_.size(),
                   "BsFlatMcSavePath: path length must match time grid");
    }
}

QuantLib::Size BsFlatMcSavePath::fixingIndex(const QuantLib::Date& date) const {
    using namespace QuantLib;
    const auto it = std::lower_bound(fixingDates_.begin(), fixingDates_.end(), date);
    QL_REQUIRE(it != fixingDates_.end() && *it == date,
               "BsFlatMcSavePath: date " << date << " is not on the simulated bank");
    return static_cast<Size>(it - fixingDates_.begin());
}

bool BsFlatMcSavePath::hasFixingDate(const QuantLib::Date& date) const {
    const auto it = std::lower_bound(fixingDates_.begin(), fixingDates_.end(), date);
    return it != fixingDates_.end() && *it == date;
}

QuantLib::Real BsFlatMcSavePath::sLevel(const QuantLib::Size pathIndex,
                                        const QuantLib::Size fixingIndex) const {
    using namespace QuantLib;
    QL_REQUIRE(pathIndex < numPaths(), "BsFlatMcSavePath: path index out of range");
    QL_REQUIRE(fixingIndex < numFixings(), "BsFlatMcSavePath: fixing index out of range");
    const Size tIdx = pathTimeIndexAtFixing_[fixingIndex];
    return paths_[pathIndex][tIdx];
}

QuantLib::Real BsFlatMcSavePath::sLevel(const QuantLib::Size pathIndex,
                                        const QuantLib::Date& date) const {
    return sLevel(pathIndex, fixingIndex(date));
}

BsFlatMcSavePath simulateBsFlatMcSavePath(const MarketData& md,
                                          const QuantLib::Date& horizonMax,
                                          const std::vector<QuantLib::Date>& fixingDates,
                                          const QuantLib::Size mcSamples,
                                          const QuantLib::BigNatural seed) {
    using namespace QuantLib;
    using namespace bs_flat_reference;

    QL_REQUIRE(mcSamples > 0, "simulateBsFlatMcSavePath: mcSamples must be positive");
    QL_REQUIRE(!fixingDates.empty(), "simulateBsFlatMcSavePath: fixingDates must be non-empty");
    QL_REQUIRE(horizonMax > md.today(), "simulateBsFlatMcSavePath: horizonMax after today");

    const auto process =
        makeQlRepoDividendBsProcess(md, horizonMax, md.spotValue());

    std::vector<Time> fixingTimes;
    fixingTimes.reserve(fixingDates.size());
    for (const Date& d : fixingDates) {
        const Time t = process->time(d);
        QL_REQUIRE(t >= 0.0, "simulateBsFlatMcSavePath: fixing must not be in the past");
        fixingTimes.push_back(t);
    }

    const TimeGrid timeGrid(fixingTimes.begin(), fixingTimes.end());

    std::vector<Size> pathTimeIndexAtFixing;
    pathTimeIndexAtFixing.reserve(fixingDates.size());
    for (const Time t : fixingTimes)
        pathTimeIndexAtFixing.push_back(timeGrid.index(t));

    typedef PseudoRandom::rsg_type rsg_type;
    typedef PathGenerator<rsg_type> path_generator_type;
    const rsg_type generator = PseudoRandom::make_sequence_generator(
        process->factors() * (timeGrid.size() - 1), seed);
    const path_generator_type pathGenerator(process, timeGrid, generator, false);

    std::vector<Path> paths;
    paths.reserve(mcSamples);

    Size pathCount = 0;
    while (pathCount < mcSamples) {
        paths.push_back(pathGenerator.next().value);
        ++pathCount;
        if (pathCount >= mcSamples)
            break;
        paths.push_back(pathGenerator.antithetic().value);
        ++pathCount;
    }

    return BsFlatMcSavePath(fixingDates, std::move(pathTimeIndexAtFixing), timeGrid,
                            std::move(paths));
}
