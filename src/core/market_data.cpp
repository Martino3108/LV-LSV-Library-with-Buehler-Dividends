/**
 * @file market_data.cpp
 * @brief `MarketData` in-memory loaders, handle build, bumps.
 */

#include "market_data.h"
#include "ql/math/interpolations/cubicinterpolation.hpp"
#include <ql/time/daycounters/business252.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace {

using namespace QuantLib;

Date parseIsoDate(const std::string& s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') {
        QL_FAIL("Invalid ISO date: " << s);
    }
    const Integer y = std::stoi(s.substr(0, 4));
    const Integer m = std::stoi(s.substr(5, 2));
    const Integer d = std::stoi(s.substr(8, 2));
    return Date(d, static_cast<Month>(m), y);
}

/** Map a year fraction to a date under Business/252 (advance business days). */
Date addYearFractionAsDays(const Date& today, Real t, const Calendar& calendar) {
    const Integer nBusiness = static_cast<Integer>(std::lround(t * 252.0));
    if (nBusiness == 0)
        return today;
    return calendar.advance(today, nBusiness, Days, Following);
}

void ensureIndex(Size idx, Size size, const std::string& what) {
    if (idx >= size) {
        QL_FAIL("Index out of range for " << what << ": idx=" << idx << " size=" << size);
    }
}

} // namespace

void MarketData::loadFromTables(const MarketDataTables& tables) {
    using namespace QuantLib;

    QL_REQUIRE(!tables.asof.empty(), "MarketDataTables: asof date required");
    QL_REQUIRE(tables.spot > 0.0, "MarketDataTables: spot must be positive");
    QL_REQUIRE(tables.rfrTenorYears.size() == tables.rfrZeroRates.size(),
               "MarketDataTables: rfr tenor/rate size mismatch");
    QL_REQUIRE(tables.repoTenorYears.size() == tables.repoZeroRates.size(),
               "MarketDataTables: repo tenor/rate size mismatch");
    QL_REQUIRE(tables.volTenorYears.size() == tables.volStrikes.size() &&
                   tables.volStrikes.size() == tables.impliedVols.size(),
               "MarketDataTables: vol long-format size mismatch");
    QL_REQUIRE(tables.dividendDates.size() == tables.dividendAmounts.size(),
               "MarketDataTables: dividend date/cash size mismatch");
    if (!tables.dividendProportional.empty()) {
        QL_REQUIRE(tables.dividendDates.size() == tables.dividendProportional.size(),
                   "MarketDataTables: dividend date/proportional size mismatch");
    }
    QL_REQUIRE(!tables.volTenorYears.empty(), "MarketDataTables: vol surface is empty");

    calendar_ = TARGET();
    dayCounter_ = Business252(calendar_);
    today_ = parseIsoDate(tables.asof);
    spotValue_ = tables.spot;

    riskFreeDates_.clear();
    riskFreeZeroRates_.clear();
    riskFreeDates_.reserve(tables.rfrTenorYears.size());
    riskFreeZeroRates_.reserve(tables.rfrZeroRates.size());
    for (Size i = 0; i < tables.rfrTenorYears.size(); ++i) {
        riskFreeDates_.push_back(addYearFractionAsDays(today_, tables.rfrTenorYears[i], calendar_));
        riskFreeZeroRates_.push_back(tables.rfrZeroRates[i]);
    }
    QL_REQUIRE(!riskFreeDates_.empty(), "MarketDataTables: risk-free curve is empty");

    repoDates_.clear();
    repoZeroRates_.clear();
    repoDates_.reserve(tables.repoTenorYears.size());
    repoZeroRates_.reserve(tables.repoZeroRates.size());
    for (Size i = 0; i < tables.repoTenorYears.size(); ++i) {
        repoDates_.push_back(addYearFractionAsDays(today_, tables.repoTenorYears[i], calendar_));
        repoZeroRates_.push_back(tables.repoZeroRates[i]);
    }
    QL_REQUIRE(!repoDates_.empty(), "MarketDataTables: repo curve is empty");

    std::vector<Real> maturityYears;
    std::vector<Real> volStrikes;
    struct VolPoint {
        Real t;
        Real k;
        Volatility v;
    };
    std::vector<VolPoint> points;
    points.reserve(tables.volTenorYears.size());
    for (Size i = 0; i < tables.volTenorYears.size(); ++i) {
        const Real t = tables.volTenorYears[i];
        const Real k = tables.volStrikes[i];
        const Volatility v = tables.impliedVols[i];
        maturityYears.push_back(t);
        volStrikes.push_back(k);
        points.push_back({t, k, v});
    }

    std::sort(maturityYears.begin(), maturityYears.end());
    maturityYears.erase(std::unique(maturityYears.begin(), maturityYears.end()), maturityYears.end());
    std::sort(volStrikes.begin(), volStrikes.end());
    volStrikes.erase(std::unique(volStrikes.begin(), volStrikes.end()), volStrikes.end());

    strikes_ = std::move(volStrikes);
    expiries_.clear();
    for (Real t : maturityYears) {
        expiries_.push_back(addYearFractionAsDays(today_, t, calendar_));
    }

    marketHorizon_ = today_;
    if (!expiries_.empty()) {
        marketHorizon_ = std::max(marketHorizon_, expiries_.back());
    }
    if (!riskFreeDates_.empty()) {
        marketHorizon_ = std::max(marketHorizon_, riskFreeDates_.back());
    }
    if (!repoDates_.empty()) {
        marketHorizon_ = std::max(marketHorizon_, repoDates_.back());
    }

    impliedVols_ = Matrix(strikes_.size(), expiries_.size(), Null<Real>());

    std::map<Real, Size> tIndex;
    for (Size i = 0; i < maturityYears.size(); ++i) {
        tIndex[maturityYears[i]] = i;
    }
    std::map<Real, Size> kIndex;
    for (Size j = 0; j < strikes_.size(); ++j) {
        kIndex[strikes_[j]] = j;
    }

    for (const auto& p : points) {
        impliedVols_[kIndex[p.k]][tIndex[p.t]] = p.v;
    }

    for (Size i = 0; i < impliedVols_.rows(); ++i) {
        for (Size j = 0; j < impliedVols_.columns(); ++j) {
            if (impliedVols_[i][j] == Null<Real>()) {
                QL_FAIL("MarketDataTables: missing vol point in matrix at row " << i << ", col "
                                                                                << j);
            }
        }
    }

    dividendDates_.clear();
    dividendAmounts_.clear();
    dividendProportional_.clear();
    dividendDates_.reserve(tables.dividendDates.size());
    dividendAmounts_.reserve(tables.dividendAmounts.size());
    dividendProportional_.reserve(tables.dividendDates.size());
    for (Size i = 0; i < tables.dividendDates.size(); ++i) {
        if (tables.dividendDates[i].empty()) {
            continue;
        }
        const Real cash = tables.dividendAmounts[i];
        const Real proportional =
            tables.dividendProportional.empty() ? Real(0.0) : tables.dividendProportional[i];
        QL_REQUIRE(cash >= 0.0, "MarketDataTables: cash dividend must be non-negative");
        QL_REQUIRE(proportional >= 0.0 && proportional < 1.0,
                   "MarketDataTables: proportional dividend must be in [0, 1)");
        QL_REQUIRE(cash > 0.0 || proportional > 0.0,
                   "MarketDataTables: each dividend date needs cash and/or proportional > 0");
        dividendDates_.push_back(parseIsoDate(tables.dividendDates[i]));
        dividendAmounts_.push_back(cash);
        dividendProportional_.push_back(proportional);
    }
    if (!dividendDates_.empty()) {
        marketHorizon_ = std::max(marketHorizon_, dividendDates_.back());
    }

    QL_REQUIRE(tables.bergomiK > 0.0, "MarketDataTables: bergomi_k must be positive");
    QL_REQUIRE(tables.bergomiNu > 0.0, "MarketDataTables: bergomi_nu must be positive");
    QL_REQUIRE(tables.bergomiRho > -1.0 && tables.bergomiRho < 1.0,
               "MarketDataTables: bergomi_rho must be in (-1, 1)");
    bergomiK_ = tables.bergomiK;
    bergomiNu_ = tables.bergomiNu;
    bergomiRho_ = tables.bergomiRho;

    buildQuantLibHandles();
}

void MarketData::loadSampleMarketSnapshot() {
    // Sample market snapshot (spot, smile, curves, discrete dividends).
    using namespace QuantLib;

    calendar_   = TARGET();
    dayCounter_ = Business252(calendar_);
    today_      = Date(1, December, 2025);

    spotValue_ = 8097.0;

    bergomiK_   = 2.0;
    bergomiNu_  = 1.0;
    bergomiRho_ = -0.7;

    const std::vector<Time> volTenorsYears = {
        0.0833, 0.25, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 5.0, 7.0, 10.0
    };
    expiries_.clear();
    expiries_.reserve(volTenorsYears.size());
    for (const Time t : volTenorsYears) {
        Date d = addYearFractionAsDays(today_, t, calendar_);
        d = calendar_.adjust(d, Following);
        expiries_.push_back(d);
    }

    const std::vector<Real> strikeMultipliers = {
        0.4, 0.6, 0.7, 0.8, 0.9, 1.0, 1.1, 1.2, 1.3, 1.5,
        1.6, 1.7, 1.8, 1.9, 2.0
    };
    strikes_.clear();
    strikes_.reserve(strikeMultipliers.size());
    for (const Real m : strikeMultipliers) {
        strikes_.push_back(spotValue_ * m);
    }

    // --- Risk-free and repo curves on requested tenor grids ---
    const std::vector<Time> rfrTenorsYears = {
        0.0055, 0.0192, 0.0833, 0.1667, 0.25, 0.5, 0.75, 1.0, 2.0, 3.0,
        4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0, 12.0, 13.0,
        14.0, 15.0, 20.0, 25.0, 30.0, 40.0, 50.0
    };
    const std::vector<Time> repoTenorsYears = {0.5, 1.0, 2.0, 5.0, 8.0, 10.0};

    riskFreeDates_.clear();
    repoDates_.clear();
    riskFreeDates_.reserve(1 + rfrTenorsYears.size());
    repoDates_.reserve(1 + repoTenorsYears.size());
    riskFreeDates_.push_back(today_);
    repoDates_.push_back(today_);
    for (const Time t : rfrTenorsYears) {
        riskFreeDates_.push_back(addYearFractionAsDays(today_, t, calendar_));
    }
    for (const Time t : repoTenorsYears) {
        repoDates_.push_back(addYearFractionAsDays(today_, t, calendar_));
    }

    const std::vector<Rate> rfrValues = {
        0.019301, 0.01929, 0.01961, 0.02011, 0.0206, 0.019248, 0.018877,
        0.020715, 0.021105, 0.02198, 0.0229, 0.023745, 0.02453, 0.0253,
        0.02604, 0.026725, 0.02735, 0.02795, 0.0285, 0.028892, 0.02943,
        0.029795, 0.030845, 0.031165, 0.03121, 0.03099, 0.030455
    };
    const std::vector<Rate> repoValues = {
        -0.0054, -0.00498, -0.00512, -0.00655, -0.0083, -0.00931
    };

    QL_REQUIRE(rfrValues.size() == rfrTenorsYears.size(),
               "rfrValues size must match rfr tenor grid size");
    QL_REQUIRE(repoValues.size() == repoTenorsYears.size(),
               "repoValues size must match repo tenor grid size");

    riskFreeZeroRates_.clear();
    repoZeroRates_.clear();
    riskFreeZeroRates_.reserve(1 + rfrValues.size());
    repoZeroRates_.reserve(1 + repoValues.size());
    riskFreeZeroRates_.push_back(rfrValues.front());
    repoZeroRates_.push_back(repoValues.front());
    riskFreeZeroRates_.insert(riskFreeZeroRates_.end(), rfrValues.begin(), rfrValues.end());
    repoZeroRates_.insert(repoZeroRates_.end(), repoValues.begin(), repoValues.end());

    marketHorizon_ = std::max(expiries_.back(),
                              std::max(riskFreeDates_.back(), repoDates_.back()));

    // --- Implied vol surface ---
    // BlackVarianceSurface expects Matrix[nStrikes][nExpiries]
    const Size nExp = expiries_.size();
    const Size nStr = strikes_.size();

    impliedVols_ = Matrix(nStr, nExp);
    const std::vector<std::vector<Volatility>> volByExpiry = {
        {0.602953, 0.537187, 0.424256, 0.318161, 0.219591, 0.138933, 0.115099, 0.128044, 0.157782, 0.204463, 0.220598, 0.233759, 0.244716, 0.253995, 0.261966}, // 0.0833
        {0.602953, 0.421098, 0.339972, 0.265417, 0.199131, 0.147037, 0.123709, 0.121700, 0.132664, 0.160394, 0.169508, 0.176719, 0.182580, 0.187446, 0.191557}, // 0.25
        {0.506700, 0.361249, 0.298839, 0.243109, 0.194719, 0.155172, 0.133166, 0.125062, 0.125696, 0.143718, 0.154374, 0.163029, 0.170213, 0.176281, 0.181480}, // 0.5
        {0.410074, 0.305644, 0.261485, 0.222535, 0.188754, 0.160138, 0.141394, 0.131018, 0.126158, 0.128702, 0.134536, 0.141729, 0.147970, 0.153232, 0.157734}, // 1.0
        {0.374516, 0.284828, 0.250008, 0.214745, 0.186712, 0.162336, 0.145022, 0.133633, 0.127957, 0.120767, 0.128271, 0.132278, 0.136425, 0.140593, 0.144599}, // 1.5
        {0.352333, 0.271886, 0.238530, 0.209592, 0.184670, 0.163415, 0.147833, 0.137101, 0.129756, 0.122665, 0.122006, 0.122827, 0.124879, 0.127953, 0.131463}, // 2.0
        {0.325976, 0.255413, 0.230627, 0.202307, 0.182723, 0.164221, 0.151437, 0.142373, 0.134629, 0.128742, 0.127051, 0.127404, 0.128800, 0.131064, 0.133778}, // 3.0
        {0.310340, 0.245570, 0.222725, 0.197864, 0.180777, 0.164621, 0.153662, 0.145819, 0.139502, 0.133460, 0.132097, 0.131982, 0.132721, 0.134176, 0.136092}, // 4.0
        {0.298960, 0.238679, 0.214822, 0.195075, 0.178830, 0.165529, 0.156075, 0.149327, 0.144375, 0.138522, 0.137142, 0.136559, 0.136642, 0.137287, 0.138407}, // 5.0
        {0.288526, 0.233290, 0.212667, 0.194076, 0.180301, 0.168355, 0.160413, 0.154703, 0.150659, 0.145018, 0.144213, 0.143573, 0.143484, 0.143859, 0.144629}, // 7.0
        {0.277741, 0.228343, 0.209434, 0.194348, 0.182507, 0.173360, 0.167443, 0.163263, 0.160085, 0.155996, 0.154820, 0.154095, 0.153747, 0.153718, 0.153961}  // 10.0
    };
    QL_REQUIRE(volByExpiry.size() == nExp, "Vol expiry rows mismatch with expiry grid size");
    for (const auto& row : volByExpiry) {
        QL_REQUIRE(row.size() == nStr, "Vol strike columns mismatch with strike grid size");
    }
    for (Size j = 0; j < nExp; ++j) {
        for (Size i = 0; i < nStr; ++i) {
            impliedVols_[i][j] = volByExpiry[j][i];
        }
    }

    // Five annual discrete cash dividends at 1Y..5Y.
    const std::vector<Real> dividendValues = {
        343.425, 228.9458, 223.6222, 225.3806, 210.553
    };
    dividendDates_.clear();
    dividendAmounts_.clear();
    dividendProportional_.clear();
    dividendDates_.reserve(dividendValues.size());
    dividendAmounts_.reserve(dividendValues.size());
    dividendProportional_.reserve(dividendValues.size());
    for (Size i = 0; i < dividendValues.size(); ++i) {
        const Integer m = static_cast<Integer>((i + 1) * 12);
        const Date exDate = calendar_.adjust(today_ + Period(m, Months), Following);
        if (exDate <= today_) {
            continue;
        }
        dividendDates_.push_back(exDate);
        dividendAmounts_.push_back(dividendValues[i]);
        dividendProportional_.push_back(0.0);
    }
    if (!dividendDates_.empty()) {
        marketHorizon_ = std::max(marketHorizon_, dividendDates_.back());
    }
    buildQuantLibHandles();
}

void MarketData::loadConstantMock() {
    // Flat Black–Scholes dataset; grid shape matches loadSampleMarketSnapshot().
    using namespace QuantLib;

    calendar_   = TARGET();
    dayCounter_ = Business252(calendar_);
    today_      = Date(1, December, 2025);

    spotValue_ = 100.0;

    bergomiK_   = 2.0;
    bergomiNu_  = 1.0;
    bergomiRho_ = -0.7;

    // Keep the same dimensionality/grid as loadSampleMarketSnapshot for clean A/B comparisons.
    expiries_ = {
        today_ + 1   * Months, today_ + 2   * Months, today_ + 3   * Months,
        today_ + 6   * Months, today_ + 9   * Months, today_ + 12  * Months,
        today_ + 18  * Months, today_ + 24  * Months, today_ + 36  * Months,
        today_ + 42  * Months, today_ + 48  * Months, today_ + 54  * Months,
        today_ + 60  * Months, today_ + 66  * Months, today_ + 72  * Months,
        today_ + 78  * Months, today_ + 84  * Months, today_ + 90  * Months,
        today_ + 96  * Months, today_ + 102 * Months, today_ + 108 * Months,
        today_ + 114 * Months, today_ + 120 * Months
    };
    for (Date& d : expiries_) {
        d = calendar_.adjust(d, Following);
    }
    strikes_ = {
        50.0, 55.0, 60.0, 65.0, 70.0, 75.0, 80.0, 85.0,
        90.0, 95.0, 100.0, 105.0, 110.0, 115.0,
        120.0, 125.0, 130.0, 135.0, 140.0, 145.0, 150.0
    };

    // Flat curves to mimic constant-rate Black-Scholes settings.
    const Rate rFlat = 0.0200;
    const Rate repoFlat = 0.0050;
    riskFreeDates_ = { today_ };
    repoDates_     = { today_ };
    riskFreeDates_.insert(riskFreeDates_.end(), expiries_.begin(), expiries_.end());
    repoDates_.insert(repoDates_.end(),         expiries_.begin(), expiries_.end());

    riskFreeZeroRates_.assign(riskFreeDates_.size(), rFlat);
    repoZeroRates_.assign(repoDates_.size(), repoFlat);

    marketHorizon_ = std::max(expiries_.back(),
                              std::max(riskFreeDates_.back(), repoDates_.back()));

    // Constant implied-vol matrix over the same strike/expiry grid.
    const Volatility sigmaFlat = 0.20;
    impliedVols_ = Matrix(strikes_.size(), expiries_.size(), sigmaFlat);

    dividendDates_.clear();
    dividendAmounts_.clear();
    dividendProportional_.clear();
    buildQuantLibHandles();
}

void MarketData::buildQuantLibHandles() {
    using namespace QuantLib;
    QL_REQUIRE(!expiries_.empty(), "MarketData requires non-empty expiries");
    QL_REQUIRE(!strikes_.empty(), "MarketData requires non-empty strikes");
    QL_REQUIRE(!riskFreeDates_.empty(), "MarketData requires risk-free curve nodes");
    QL_REQUIRE(!repoDates_.empty(), "MarketData requires repo curve nodes");
    // Note: QuantLib's global Settings::evaluationDate is deliberately NOT touched here.
    // Every term structure and FD process below is anchored to an explicit reference
    // date (today_), so multiple snapshots can coexist without global-state races.

    spot_ = Handle<Quote>(ext::make_shared<SimpleQuote>(spotValue_));

    auto rfrCurve = ext::make_shared<InterpolatedZeroCurve<Cubic>>(
        riskFreeDates_, riskFreeZeroRates_, dayCounter_, calendar_);
    rfrCurve->enableExtrapolation();
    riskFreeTs_ = Handle<YieldTermStructure>(rfrCurve);

    auto repoCurve = ext::make_shared<InterpolatedZeroCurve<Cubic>>(
        repoDates_, repoZeroRates_, dayCounter_, calendar_);
    repoCurve->enableExtrapolation();
    repoTs_ = Handle<YieldTermStructure>(repoCurve);

    auto blackSurface = ext::make_shared<BlackVarianceSurface>(
        today_, calendar_, expiries_, strikes_, impliedVols_, dayCounter_,
        BlackVarianceSurface::ConstantExtrapolation, BlackVarianceSurface::ConstantExtrapolation);
    blackSurface->setInterpolation<Bicubic>();
    blackSurface->enableExtrapolation();
    blackVolTs_ = Handle<BlackVolTermStructure>(blackSurface);
}

void MarketData::bumpSpot(Real bump) {
    spotValue_ += bump;
    buildQuantLibHandles();
}

void MarketData::bumpRiskFreeNode(Size idx, Rate bump) {
    ensureIndex(idx, riskFreeZeroRates_.size(), "riskFreeZeroRates");
    riskFreeZeroRates_[idx] += bump;
    buildQuantLibHandles();
}

void MarketData::bumpRepoNode(Size idx, Rate bump) {
    ensureIndex(idx, repoZeroRates_.size(), "repoZeroRates");
    repoZeroRates_[idx] += bump;
    buildQuantLibHandles();
}

void MarketData::bumpImpliedVolNode(Size strikeIdx, Size expiryIdx, Volatility bump) {
    ensureIndex(strikeIdx, impliedVols_.rows(), "impliedVols rows");
    ensureIndex(expiryIdx, impliedVols_.columns(), "impliedVols columns");
    impliedVols_[strikeIdx][expiryIdx] += bump;
    buildQuantLibHandles();
}

void MarketData::bumpDividendNode(Size idx, Real bump) {
    ensureIndex(idx, dividendAmounts_.size(), "dividendAmounts");
    dividendAmounts_[idx] += bump;
}