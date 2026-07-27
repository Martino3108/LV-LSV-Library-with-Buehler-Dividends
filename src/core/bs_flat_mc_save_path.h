/**
 * @file bs_flat_mc_save_path.h
 * @brief Shared flat BS MC path bank in S for pipeline sanity pricing.
 */

#ifndef BS_FLAT_MC_SAVE_PATH_H
#define BS_FLAT_MC_SAVE_PATH_H

#include <ql/methods/montecarlo/path.hpp>
#include <ql/timegrid.hpp>
#include <ql/quantlib.hpp>
#include <vector>

class MarketData;

/** @brief Simulated flat BS spot paths on a shared fixing grid (S only). */
class BsFlatMcSavePath {
public:
    BsFlatMcSavePath() = default;
    BsFlatMcSavePath(std::vector<QuantLib::Date> fixingDates,
                     std::vector<QuantLib::Size> pathTimeIndexAtFixing,
                     QuantLib::TimeGrid timeGrid,
                     std::vector<QuantLib::Path> paths);

    BsFlatMcSavePath(const BsFlatMcSavePath&) = delete;
    BsFlatMcSavePath& operator=(const BsFlatMcSavePath&) = delete;
    BsFlatMcSavePath(BsFlatMcSavePath&&) = default;
    BsFlatMcSavePath& operator=(BsFlatMcSavePath&&) = default;
    ~BsFlatMcSavePath() = default;

    const std::vector<QuantLib::Date>& fixingDates() const { return fixingDates_; }
    QuantLib::Size numPaths() const { return paths_.size(); }
    QuantLib::Size numFixings() const { return fixingDates_.size(); }

    QuantLib::Size fixingIndex(const QuantLib::Date& date) const;
    bool hasFixingDate(const QuantLib::Date& date) const;
    QuantLib::Real sLevel(QuantLib::Size pathIndex, QuantLib::Size fixingIndex) const;
    QuantLib::Real sLevel(QuantLib::Size pathIndex, const QuantLib::Date& date) const;

private:
    std::vector<QuantLib::Date> fixingDates_;
    std::vector<QuantLib::Size> pathTimeIndexAtFixing_;
    QuantLib::TimeGrid timeGrid_;
    std::vector<QuantLib::Path> paths_;
};

/** @brief One antithetic MC bank on @p fixingDates (daily business grid for sanity). */
BsFlatMcSavePath simulateBsFlatMcSavePath(const MarketData& md,
                                          const QuantLib::Date& horizonMax,
                                          const std::vector<QuantLib::Date>& fixingDates,
                                          QuantLib::Size mcSamples,
                                          QuantLib::BigNatural seed);

#endif
