/**
 * @file buehler_iv_x_arbitrage.cpp
 */

#include "buehler_iv_x_arbitrage.h"
#include "benchmark_numeric.h"
#include "market_data.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>
#include <ql/pricingengines/blackformula.hpp>
#include <ql/quantlib.hpp>

namespace {

QuantLib::Real normalizedCallPureX(const QuantLib::Handle<QuantLib::BlackVolTermStructure>& volTs,
                                     const QuantLib::Date& refDate,
                                     const QuantLib::DayCounter& dc,
                                     const QuantLib::Date& expiry,
                                     QuantLib::Real kx) {
    using namespace QuantLib;
    const Time t = dc.yearFraction(refDate, expiry);
    if (t <= 1.0e-14) {
        return std::max(1.0 - kx, 0.0);
    }
    const Volatility sigma = volTs->blackVol(expiry, kx, true);
    const Real stdDev = sigma * std::sqrt(t);
    return blackFormula(QuantLib::Option::Call, kx, 1.0, stdDev, 1.0);
}

QuantLib::Date dateFromYearFraction(const BuehlerModel& b, QuantLib::Time T) {
    return dateFromAct365YearFraction(b.today(), T, b.calendar());
}

} // namespace

BuehlerImpliedVolXArbitrageReport check_static_arbitrage(
    const BuehlerModel& buehler,
    QuantLib::Size nTimeSamples,
    QuantLib::Size nStrikeSamples,
    double tolButterfly,
    double tolCalendar,
    bool verbose) {
    using namespace QuantLib;
    BuehlerImpliedVolXArbitrageReport rep;
    const Handle<BlackVolTermStructure> volTs = buehler.impliedVolXBicubicTs();
    QL_REQUIRE(!volTs.empty(), "BuehlerModel: impliedVolXBicubicTs empty; call calibration() first");
    const Date& ref = buehler.today();
    const DayCounter& dc = buehler.dayCounter();
    const auto& expiries = buehler.preBicubicImpliedVolXExpiries();
    const auto& kxGrid = buehler.preBicubicImpliedVolXKxGrid();
    const Real xMin = *std::min_element(kxGrid.begin(), kxGrid.end());
    const Real xMax = *std::max_element(kxGrid.begin(), kxGrid.end());
    QL_REQUIRE(xMax > xMin && std::isfinite(xMin) && std::isfinite(xMax),
               "Buehler implied-vol X arbitrage: invalid strike range");
    // Align strike tests with the fixed-LV tabulation: only the synthetic band
    // [max_T kx(K_min), min_T kx(K_max)] after crop.
    const Real kxTabLo =
        buehler.denseXStrikes().empty() ? xMin : buehler.denseXStrikes().front();
    const Real kxTabHi =
        buehler.denseXStrikes().empty() ? xMax : buehler.denseXStrikes().back();
    const bool restrictKxLo = (kxTabLo > 1.0e-10 && kxTabLo <= xMax);
    const bool restrictKxHi = (kxTabHi < QL_MAX_REAL && kxTabHi >= xMin);
    const Real xEffMin = restrictKxLo ? std::max(xMin, kxTabLo) : xMin;
    const Real xEffMax = restrictKxHi ? std::min(xMax, kxTabHi) : xMax;
    QL_REQUIRE(xEffMax > xEffMin,
               "Buehler implied-vol X arbitrage: empty strike range after LV tab crop");

    const Time tMin = dc.yearFraction(ref, expiries.front());
    const Time tMax = dc.yearFraction(ref, expiries.back());
    const Time tLo = std::max(tMin, 1.0 / 52.0);
    const Time tHi = std::max(tLo + 1.0 / 52.0, tMax);

    auto sampleTimes = [&]() {
        std::vector<Time> ts;
        ts.reserve(nTimeSamples);
        const double tLoD = benchmarkToDouble(tLo);
        const double tHiD = benchmarkToDouble(tHi);
        for (Size i = 0; i < nTimeSamples; ++i) {
            const double w = (nTimeSamples <= 1)
                                 ? 0.0
                                 : static_cast<double>(i) / static_cast<double>(nTimeSamples - 1);
            ts.push_back(static_cast<Time>(tLoD + w * (tHiD - tLoD)));
        }
        return ts;
    };
    const std::vector<Time> timeGrid = sampleTimes();

    auto sampleStrikes = [&](Real margin) {
        std::vector<Real> xs;
        xs.reserve(nStrikeSamples);
        const Real lo = xEffMin * (1.0 + margin);
        const Real hi = xEffMax * (1.0 - margin);
        QL_REQUIRE(hi > lo, "Buehler implied-vol X arbitrage: empty strike interior after margin");
        for (Size i = 0; i < nStrikeSamples; ++i) {
            const double w = (nStrikeSamples <= 1)
                                 ? 0.0
                                 : static_cast<double>(i) / static_cast<double>(nStrikeSamples - 1);
            const double logLo = std::log(benchmarkToDouble(lo));
            const double logHi = std::log(benchmarkToDouble(hi));
            xs.push_back(static_cast<Real>(std::exp(logLo + w * (logHi - logLo))));
        }
        return xs;
    };
    const Real interiorMargin = 0.02;
    const std::vector<Real> strikeGrid = sampleStrikes(interiorMargin);

    const Real relHx = 5.0e-4;

    if (verbose) {
        std::cout << std::fixed << std::setprecision(6);
        std::cout << "\n=== Buehler implied vol X: static arbitrage (butterfly + calendar) ===\n";
        if (restrictKxLo || restrictKxHi) {
            std::cout << "X strike samples restricted to kx in [" << xEffMin << ", " << xEffMax
                      << "] (matches dense fixed-LV kx tab after synthetic crop; bicubic grid was ["
                      << xMin << ", " << xMax << "])\n";
        }
        std::cout << "X range used [" << xEffMin << ", " << xEffMax << "] | T range [" << tLo << ", "
                  << tHi << "] yr\n";
    }

    rep.minButterfly = QL_MAX_REAL;
    for (const Time T : timeGrid) {
        const Date expiry = dateFromYearFraction(buehler, T);
        const Time tAct = dc.yearFraction(ref, expiry);
        if (tAct <= 1.0e-12) {
            continue;
        }
        const double kHardLo = restrictKxLo ? benchmarkToDouble(kxTabLo) : 0.0;
        const double kHardHi =
            restrictKxHi ? benchmarkToDouble(kxTabHi) : std::numeric_limits<double>::infinity();
        for (const auto& kRaw : strikeGrid) {
            const double k = benchmarkToDouble(kRaw);
            const double h = std::max(benchmarkToDouble(relHx) * k, 1.0e-6);
            if (k - h <= kHardLo || k + h >= kHardHi) {
                continue;
            }
            const Real cm = normalizedCallPureX(volTs, ref, dc, expiry, static_cast<Real>(k - h));
            const Real c0 = normalizedCallPureX(volTs, ref, dc, expiry, static_cast<Real>(k));
            const Real cp = normalizedCallPureX(volTs, ref, dc, expiry, static_cast<Real>(k + h));
            const double d2 = benchmarkToDouble(cm - 2.0 * c0 + cp) / (h * h);
            ++rep.nSamplesButterfly;
            rep.minButterfly = std::min(rep.minButterfly, d2);
            if (d2 < tolButterfly) {
                ++rep.violationsButterfly;
                if (verbose) {
                    std::cout << "  [butterfly] T=" << tAct << " k=" << k << " d2xxC=" << d2 << "\n";
                }
            }
        }
    }
    if (rep.minButterfly == QL_MAX_REAL) {
        rep.minButterfly = 0.0;
    }

    rep.minCalendar = QL_MAX_REAL;
    for (Size it = 1; it + 1 < timeGrid.size(); ++it) {
        const Time Tm = timeGrid[it - 1];
        const Time T0 = timeGrid[it];
        const Time Tp = timeGrid[it + 1];
        const Date dm = dateFromYearFraction(buehler, Tm);
        const Date d0 = dateFromYearFraction(buehler, T0);
        const Date dp = dateFromYearFraction(buehler, Tp);
        if (dm >= d0 || d0 >= dp) {
            continue;
        }
        const Real dTm = dc.yearFraction(ref, dm);
        const Real dT0 = dc.yearFraction(ref, d0);
        const Real dTp = dc.yearFraction(ref, dp);
        if (dT0 <= 1.0e-12) {
            continue;
        }
        for (const auto& kRaw : strikeGrid) {
            const double k = benchmarkToDouble(kRaw);
            const Real cm = normalizedCallPureX(volTs, ref, dc, dm, static_cast<Real>(k));
            const Real c0 = normalizedCallPureX(volTs, ref, dc, d0, static_cast<Real>(k));
            const Real cp = normalizedCallPureX(volTs, ref, dc, dp, static_cast<Real>(k));
            const double dTspan = benchmarkToDouble(dTp - dTm);
            if (dTspan <= 1.0e-14) {
                continue;
            }
            const double dCdT = benchmarkToDouble(cp - cm) / dTspan;
            ++rep.nSamplesCalendar;
            rep.minCalendar = std::min(rep.minCalendar, dCdT);
            if (dCdT < tolCalendar) {
                ++rep.violationsCalendar;
                if (verbose) {
                    std::cout << "  [calendar] k=" << k << " T=" << dT0 << " dT_C=" << dCdT << "\n";
                }
            }
        }
    }
    if (rep.nSamplesCalendar == 0 || rep.minCalendar == QL_MAX_REAL) {
        rep.minCalendar = 0.0;
    }

    if (verbose) {
        std::cout << "Samples: butterfly=" << rep.nSamplesButterfly
                  << " calendar=" << rep.nSamplesCalendar << "\n";
        std::cout << "Violations: butterfly=" << rep.violationsButterfly
                  << " calendar=" << rep.violationsCalendar << "\n";
        std::cout << "Worst: min d2xxC=" << rep.minButterfly << " min dT_C=" << rep.minCalendar
                  << "\n";
        std::cout << "Gate: fail if violations > "
                  << (100.0 * BuehlerImpliedVolXArbitrageReport::kMaxViolationFraction)
                  << "% of samples or minButterfly < "
                  << BuehlerImpliedVolXArbitrageReport::kMinButterflyFloor << "\n";
        std::cout << "Overall: " << (rep.allPassed() ? "PASS" : "FAIL") << "\n";
    }

    return rep;
}
