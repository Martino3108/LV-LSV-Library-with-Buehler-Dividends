/**
 * @file market_data.h
 * @brief Market curves, spot, vol surface, and dividends (in-memory loaders).
 */

#ifndef MARKET_DATA_H
#define MARKET_DATA_H

#include <string>
#include <vector>
#include <ql/quantlib.hpp>

/** @brief Normalized market tables passed to @c MarketData::loadFromTables. */
struct MarketDataTables {
    std::string asof;
    QuantLib::Real spot = 0.0;
    std::vector<QuantLib::Real> rfrTenorYears;
    std::vector<QuantLib::Rate> rfrZeroRates;
    std::vector<QuantLib::Real> repoTenorYears;
    std::vector<QuantLib::Rate> repoZeroRates;
    /** Long-format vol grid: parallel @c volTenorYears, @c volStrikes, @c impliedVols. */
    std::vector<QuantLib::Real> volTenorYears;
    std::vector<QuantLib::Real> volStrikes;
    std::vector<QuantLib::Volatility> impliedVols;
    std::vector<std::string> dividendDates;
    std::vector<QuantLib::Real> dividendAmounts;
    /** Bergomi 1F stochastic-vol inputs (LSV dynamics): mean reversion, vol-of-vol, spot-vol correlation. */
    QuantLib::Real bergomiK = 2.0;
    QuantLib::Real bergomiNu = 1.0;
    QuantLib::Real bergomiRho = -0.7;
};

/** @brief In-memory market dataset; builds QuantLib term structures and Black vol. */
class MarketData {
public:
    MarketData() = default;

    /** @brief Build from normalized tables (e.g. from Python/JSON via @c loadFromTables). */
    void loadFromTables(const MarketDataTables& tables);

    /** @brief Realistic sample snapshot: smile, curves, discrete cash dividends. */
    void loadSampleMarketSnapshot();
    /** @brief Flat vol and curves for Black–Scholes regression (grid aligned with sample snapshot). */
    void loadConstantMock();

    void bumpSpot(QuantLib::Real bump);
    void bumpRiskFreeNode(QuantLib::Size idx, QuantLib::Rate bump);
    void bumpRepoNode(QuantLib::Size idx, QuantLib::Rate bump);
    void bumpImpliedVolNode(QuantLib::Size strikeIdx, QuantLib::Size expiryIdx, QuantLib::Volatility bump);
    void bumpDividendNode(QuantLib::Size idx, QuantLib::Real bump);

    const QuantLib::Date& today() const { return today_; }
    const QuantLib::Calendar& calendar() const { return calendar_; }
    const QuantLib::DayCounter& dayCounter() const { return dayCounter_; }
    const QuantLib::Date& marketHorizon() const { return marketHorizon_; }
    const std::vector<QuantLib::Date>& expiries() const { return expiries_; }
    const std::vector<QuantLib::Real>& strikes() const { return strikes_; }
    const QuantLib::Matrix& impliedVols() const { return impliedVols_; }

    const QuantLib::Handle<QuantLib::Quote>& spot() const { return spot_; }
    const QuantLib::Handle<QuantLib::YieldTermStructure>& riskFreeTs() const { return riskFreeTs_; }
    const QuantLib::Handle<QuantLib::YieldTermStructure>& repoTs() const { return repoTs_; }
    const std::vector<QuantLib::Date>& dividendDates() const { return dividendDates_; }
    const std::vector<QuantLib::Real>& dividendAmounts() const { return dividendAmounts_; }
    const QuantLib::Handle<QuantLib::BlackVolTermStructure>& blackVolTs() const { return blackVolTs_; }
    QuantLib::Real spotValue() const { return spotValue_; }
    const std::vector<QuantLib::Date>& riskFreeDates() const { return riskFreeDates_; }
    const std::vector<QuantLib::Rate>& riskFreeZeroRates() const { return riskFreeZeroRates_; }
    const std::vector<QuantLib::Date>& repoDates() const { return repoDates_; }
    const std::vector<QuantLib::Rate>& repoZeroRates() const { return repoZeroRates_; }

    /** Bergomi 1F inputs (model parameters, not calibrated). */
    QuantLib::Real bergomiK() const { return bergomiK_; }
    QuantLib::Real bergomiNu() const { return bergomiNu_; }
    QuantLib::Real bergomiRho() const { return bergomiRho_; }

private:
    QuantLib::Date today_;
    QuantLib::Calendar calendar_;
    QuantLib::DayCounter dayCounter_;
    QuantLib::Date marketHorizon_;
    std::vector<QuantLib::Date> expiries_;
    std::vector<QuantLib::Real> strikes_;
    QuantLib::Matrix impliedVols_;

    QuantLib::Handle<QuantLib::Quote> spot_;
    QuantLib::Handle<QuantLib::YieldTermStructure> riskFreeTs_;
    QuantLib::Handle<QuantLib::YieldTermStructure> repoTs_;
    QuantLib::Real spotValue_ = 0.0;
    std::vector<QuantLib::Date> riskFreeDates_;
    std::vector<QuantLib::Rate> riskFreeZeroRates_;
    std::vector<QuantLib::Date> repoDates_;
    std::vector<QuantLib::Rate> repoZeroRates_;
    std::vector<QuantLib::Date> dividendDates_;
    std::vector<QuantLib::Real> dividendAmounts_;
    QuantLib::Handle<QuantLib::BlackVolTermStructure> blackVolTs_;
    QuantLib::Real bergomiK_ = 2.0;
    QuantLib::Real bergomiNu_ = 1.0;
    QuantLib::Real bergomiRho_ = -0.7;

    void buildQuantLibHandles();
};

#endif
