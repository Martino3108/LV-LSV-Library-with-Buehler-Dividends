/**
 * @file verify_fit.cpp
 */

#include "verify_fit.h"
#include "benchmark_log.h"
#include "benchmark_numeric.h"
#include "buehler_fixing_save_path.h"
#include "buehler_model.h"
#include "european_mc_buehler_option.h"
#include "lv_european_fd_buehler_option.h"
#include "market_data.h"
#include <ql/instruments/payoffs.hpp>
#include <ql/instruments/vanillaoption.hpp>
#include <ql/pricingengines/blackformula.hpp>
#include <ql/pricingengines/vanilla/analyticeuropeanengine.hpp>
#include <ql/processes/blackscholesprocess.hpp>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <vector>
#include <ql/quantlib.hpp>

namespace {

/** Row for Buehler S round-trip: P(0,T)*A(T)*C_X^LV vs C_S^BS from market σ_S and F_S=Buehler forward. */
struct BuehlerLvBsSCompareRow {
    QuantLib::Date expiry;
    QuantLib::Real strikeS = 0.0;
    double lvPriceS = 0.0;
    double refPriceS = 0.0;
    double absErrPxS = 0.0;
};

/** Implied σ_S from LV round-trip price vs market σ_S at (T, K_S). */
struct BuehlerSigmaSFromLvRow {
    QuantLib::Date expiry;
    QuantLib::Real strikeS = 0.0;
    double sigmaMarketS = 0.0;
    double sigmaImpliedS = 0.0;
    double absErrIvBp = 0.0;
};
} // namespace

void export_lv_fixed_x_csv(const BuehlerModel& buehler,
                           const std::string& output_path,
                           const bool verbose) {
    using namespace QuantLib;
    const Handle<LocalVolTermStructure>& fixedLvTs = buehler.fixedPureLocalVolTs();
    QL_REQUIRE(!fixedLvTs.empty(), "export_lv_fixed_x_csv: empty fixed pure-X local vol handle");

    constexpr Size kLvTPoints = 500;
    constexpr Size kLvKxPoints = 1000;
    static_assert(kLvTPoints * kLvKxPoints == 500000, "LV export grid must be 500k cells");
    constexpr double kxExportLo = 0.0;
    constexpr double kxExportHi = 2.5;
    constexpr double tExportLo = 0.0;
    constexpr double tExportHiYears = 10.0;

    const std::filesystem::path outPath(output_path);
    if (const std::filesystem::path parent = outPath.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }

    std::ofstream out(output_path);
    QL_REQUIRE(out.good(), "export_lv_fixed_x_csv: cannot open " << output_path);
    out << std::setprecision(17);
    out << "t,kx,sigma\n";

    for (Size iT = 0; iT < kLvTPoints; ++iT) {
        const double tCsv = (kLvTPoints == 1)
                                ? tExportLo
                                : tExportLo + (tExportHiYears - tExportLo) *
                                                  static_cast<double>(iT) /
                                                  static_cast<double>(kLvTPoints - 1);
        const Time tEval = static_cast<Time>(tCsv);
        for (Size iK = 0; iK < kLvKxPoints; ++iK) {
            const double kxCsv = (kLvKxPoints == 1)
                                     ? kxExportLo
                                     : kxExportLo + (kxExportHi - kxExportLo) *
                                                        static_cast<double>(iK) /
                                                        static_cast<double>(kLvKxPoints - 1);
            const Real kxEval = std::max(static_cast<Real>(kxCsv), 1.0e-12);
            const Volatility sigma = fixedLvTs->localVol(tEval, kxEval, true);
            out << tCsv << ',' << kxCsv << ',' << sigma << '\n';
        }
    }

    if (verbose) {
        std::cout << "Wrote " << output_path << " (" << (kLvTPoints * kLvKxPoints) << " rows)\n";
    }
}

LvFixedXMarketGrid collect_lv_fixed_x_market_grid(const MarketData& md,
                                                  const BuehlerModel& buehler) {
    using namespace QuantLib;
    LvFixedXMarketGrid grid;
    QL_REQUIRE(!md.expiries().empty(), "collect_lv_fixed_x_market_grid: empty expiries");
    QL_REQUIRE(!md.strikes().empty(), "collect_lv_fixed_x_market_grid: empty strikes");
    QL_REQUIRE(!buehler.fixedPureLocalVolTs().empty(),
               "collect_lv_fixed_x_market_grid: run BuehlerModel::calibration first");

    const Handle<LocalVolTermStructure>& fixedLv = buehler.fixedPureLocalVolTs();
    grid.rows.reserve(md.expiries().size() * md.strikes().size());

    for (Size j = 0; j < md.expiries().size(); ++j) {
        const Date expiry = md.expiries()[j];
        const Time t = md.dayCounter().yearFraction(md.today(), expiry);
        if (t <= 0.0) {
            continue;
        }
        for (Size i = 0; i < md.strikes().size(); ++i) {
            const Real strikeS = md.strikes()[i];
            const Real kx = LvEuropeanFdBuehlerOption::pureXStrikeFromSpot(buehler, expiry, strikeS);
            if (!(kx > 0.0) || !std::isfinite(kx)) {
                continue;
            }
            const Volatility sigma = fixedLv->localVol(t, kx, true);
            if (!std::isfinite(sigma) || sigma <= 0.0) {
                continue;
            }

            LvFixedXMarketRow row;
            row.expiry = expiry;
            row.strikeS = strikeS;
            row.tenorYears = t;
            row.kx = kx;
            row.sigmaLocX = sigma;
            grid.rows.push_back(row);

            if (grid.rows.size() == 1) {
                grid.marketTMin = grid.marketTMax = t;
                grid.marketKxMin = grid.marketKxMax = kx;
            } else {
                grid.marketTMin = std::min(grid.marketTMin, t);
                grid.marketTMax = std::max(grid.marketTMax, t);
                grid.marketKxMin = std::min(grid.marketKxMin, kx);
                grid.marketKxMax = std::max(grid.marketKxMax, kx);
            }
        }
    }

    QL_REQUIRE(!grid.rows.empty(), "collect_lv_fixed_x_market_grid: no valid market pillars");

    if (!buehler.denseXStrikes().empty()) {
        grid.tabulatedKxMin = buehler.denseXStrikes().front();
        grid.tabulatedKxMax = buehler.denseXStrikes().back();
    }
    if (!buehler.denseExpiries().empty()) {
        grid.tabulatedTMin =
            md.dayCounter().yearFraction(md.today(), buehler.denseExpiries().front());
        grid.tabulatedTMax =
            md.dayCounter().yearFraction(md.today(), buehler.denseExpiries().back());
        if (grid.tabulatedTMin < 0.0) {
            grid.tabulatedTMin = 0.0;
        }
    }

    return grid;
}

LvFixedXTabulatedGrid collect_lv_fixed_x_tabulated_grid(const BuehlerModel& buehler) {
    using namespace QuantLib;
    LvFixedXTabulatedGrid grid;
    QL_REQUIRE(!buehler.denseXStrikes().empty(),
               "collect_lv_fixed_x_tabulated_grid: run BuehlerModel::calibration first");
    QL_REQUIRE(!buehler.denseExpiries().empty(),
               "collect_lv_fixed_x_tabulated_grid: empty dense expiries");
    const Matrix& lv = buehler.denseLocalVolXGrid();
    QL_REQUIRE(lv.rows() == buehler.denseXStrikes().size(),
               "collect_lv_fixed_x_tabulated_grid: dense LV row count mismatch");
    QL_REQUIRE(lv.columns() == buehler.denseExpiries().size(),
               "collect_lv_fixed_x_tabulated_grid: dense LV column count mismatch");

    grid.kx.assign(buehler.denseXStrikes().begin(), buehler.denseXStrikes().end());
    grid.tenorYears.reserve(buehler.denseExpiries().size());
    for (const Date& expiry : buehler.denseExpiries()) {
        grid.tenorYears.push_back(
            static_cast<double>(buehler.dayCounter().yearFraction(buehler.today(), expiry)));
    }

    grid.sigma.resize(lv.rows());
    for (Size i = 0; i < lv.rows(); ++i) {
        grid.sigma[i].resize(lv.columns());
        for (Size j = 0; j < lv.columns(); ++j) {
            grid.sigma[i][j] = static_cast<double>(lv[i][j]);
        }
    }
    return grid;
}

QuantLib::Real buehlerMarketEuropeanCallPriceInS(const MarketData& md,
                                                const BuehlerModel& buehler,
                                                const QuantLib::Date& expiry,
                                                QuantLib::Real strikeS) {
    using namespace QuantLib;
    QL_REQUIRE(!md.blackVolTs().empty(), "buehlerMarketEuropeanCallPriceInS: empty market Black vol");
    const Time t = md.dayCounter().yearFraction(md.today(), expiry);
    QL_REQUIRE(t > 0.0, "buehlerMarketEuropeanCallPriceInS: non-positive time to expiry");
    const Volatility sigmaMarketS = md.blackVolTs()->blackVol(expiry, strikeS, true);
    const Real forwardS = buehler.forward0T(expiry);
    const Real discountS = buehler.riskFreeTs()->discount(expiry);
    const Real stdDevS = sigmaMarketS * std::sqrt(t);
    return blackFormula(QuantLib::Option::Call, strikeS, forwardS, stdDevS, discountS);
}

void verify_LV_BS_consistency(const MarketData& md,
                              const BuehlerModel& buehler,
                              const QuantLib::Size tGridPerYear,
                              const QuantLib::Size xGrid,
                              const bool verbose) {
    ScopedTeeLog logCapture("out_verify.txt");
    using namespace QuantLib;
    // The per-node BS reference below prices a QuantLib VanillaOption, whose expiry
    // check reads the global evaluation date; pin it (RAII-restored) to the snapshot.
    const SavedSettings savedQlSettings;
    Settings::instance().evaluationDate() = md.today();
    QL_REQUIRE(!md.expiries().empty(), "MarketData not initialized: expiries are empty");
    QL_REQUIRE(!md.strikes().empty(), "MarketData not initialized: strikes are empty");
    QL_REQUIRE(!buehler.pureBlackVolTs().empty(),
               "verify_LV_BS_consistency: run BuehlerModel::calibration (pure Black vol)");
    QL_REQUIRE(!buehler.fixedPureLocalVolTs().empty(),
               "verify_LV_BS_consistency: run BuehlerModel::calibration (fixed pure-X LV)");
    QL_REQUIRE(!md.riskFreeTs().empty(), "verify_LV_BS_consistency: empty market risk-free curve");
    QL_REQUIRE(!md.repoTs().empty(), "verify_LV_BS_consistency: empty market repo curve");
    QL_REQUIRE(!md.blackVolTs().empty(), "verify_LV_BS_consistency: empty market Black vol surface");
    QL_REQUIRE(!md.spot().empty(), "verify_LV_BS_consistency: empty market spot");

    auto xCurve = ext::make_shared<FlatForward>(md.today(), 0.0, md.dayCounter());
    xCurve->enableExtrapolation();
    const Handle<Quote> testSpot(ext::make_shared<SimpleQuote>(1.0));
    const Handle<YieldTermStructure> xTs(xCurve);
    const Handle<BlackVolTermStructure> testBlackVolTs = buehler.pureBlackVolTs();
    QL_REQUIRE(tGridPerYear > 0, "verify_LV_BS_consistency: tGridPerYear must be positive");
    QL_REQUIRE(xGrid > 0, "verify_LV_BS_consistency: xGrid must be positive");
    const Size strikeEdgePadding = 0; // include all strikes
    const Size expiryEdgePadding = 0; // include all maturities
    constexpr double kMinEuropeanCallPrice = 1.0e-9;
    constexpr double kMaxEuropeanCallPriceAbs = 1.0e8;
    double sumAbsErrPx = 0.0;
    double sumAbsErrSpotBp = 0.0;
    double sumAbsErrIvBp = 0.0;
    std::vector<double> absErrPxValues;
    std::vector<double> absErrSpotBpValues;
    std::vector<double> absErrIvBpValues;
    std::vector<BuehlerLvBsSCompareRow> buehlerSCompareRows;
    std::vector<BuehlerSigmaSFromLvRow> sigmaSFromLvRows;
    Size nPoints = 0;
    Size nIvPoints = 0;
    Size nCandidatePairs = 0;
    Size nSkipT = 0;
    Size nSkipA = 0;
    Size nSkipKxNonPos = 0;
    Size nSkipKxBelowTab = 0;
    Size nSkipLvPrice = 0;
    Size nSkipBsPrice = 0;
    Size nSkipTinyCall = 0;
    Size nSkipFdFailed = 0;
    const Real kxTabLo =
        buehler.denseXStrikes().empty() ? 0.0 : buehler.denseXStrikes().front();
    const Size earlyExpirySkip = 1;
    const Size expiryStart = expiryEdgePadding + earlyExpirySkip;
    if (verbose) {
        std::cout << std::fixed << std::setprecision(6);
        std::cout << "\n=== LocalVol Price Consistency Test (Buehler pure X) ===\n";
        std::cout << "FD grid: " << tGridPerYear << " t-steps/year x " << xGrid
                  << " x-nodes; w^2 front-loaded rollback times, Crank-Nicolson\n";
        std::cout << "Today: " << md.today() << "\n";
        std::cout << "Market grid: " << md.strikes().size() << " strikes x " << md.expiries().size()
                  << " expiries | verify loop skips earliest " << earlyExpirySkip
                  << " pillar(s) | kxTabLo=" << kxTabLo << "\n";
        std::cout << "Expiry           | Strike | LVPrice | BSPrice | IV_real | IV_fit\n";
        std::cout << "-------------------------------------------------------------------\n";
    }
    QL_REQUIRE(md.strikes().size() > 2 * strikeEdgePadding,
               "Not enough strikes for requested edge padding");
    QL_REQUIRE(md.expiries().size() > 2 * expiryEdgePadding,
               "Not enough expiries for requested edge padding");
    QL_REQUIRE(expiryStart < md.expiries().size() - expiryEdgePadding,
               "Not enough expiries after skipping early maturities");
    constexpr double kPriceScaleForSpotBpX = 1.0; // NPV in X with X_0 = 1
    for (Size j = expiryStart; j < md.expiries().size() - expiryEdgePadding; ++j) {
        const Date expiry = md.expiries()[j];
        const Time t = md.dayCounter().yearFraction(md.today(), expiry);
        if (t <= 0.0) {
            nSkipT += md.strikes().size() - 2 * strikeEdgePadding;
            continue;
        }
        for (Size i = strikeEdgePadding; i < md.strikes().size() - strikeEdgePadding; ++i) {
            const Real strikeS = md.strikes()[i];
            ++nCandidatePairs;
            const Real D = buehler.dividendCarry0T(expiry);
            const Real A = buehler.forward0T(expiry) - D;
            if (A <= 0.0) {
                ++nSkipA;
                continue;
            }
            const Real strike = (strikeS - D) / A;
            if (strike <= 0.0) {
                ++nSkipKxNonPos;
                continue;
            }
            if (!buehler.denseXStrikes().empty()) {
                if (strike < kxTabLo - 1.0e-12) {
                    ++nSkipKxBelowTab;
                    continue;
                }
            }
            const double paScaleS = benchmarkToDouble(buehler.riskFreeTs()->discount(expiry) * A);
            const Volatility surfaceIv = testBlackVolTs->blackVol(expiry, strike, true);

            OptionContractParams fdParams;
            fdParams.expiry = expiry;
            fdParams.strike = strike;
            fdParams.isCall = true;
            Real lvPrice = Null<Real>();
            try {
                lvPrice =
                    LvEuropeanFdBuehlerOption(fdParams, BuehlerOptionPriceSpace::X, tGridPerYear,
                                                xGrid)
                        .price(buehler);
            } catch (const std::exception&) {
                ++nSkipFdFailed;
                continue;
            }

            const ext::shared_ptr<StrikedTypePayoff> payoff =
                ext::make_shared<PlainVanillaPayoff>(QuantLib::Option::Call, strike);
            const ext::shared_ptr<Exercise> exercise = ext::make_shared<EuropeanExercise>(expiry);
            VanillaOption option(payoff, exercise);
            const double lvPriceD = benchmarkToDouble(lvPrice);
            if (!std::isfinite(lvPriceD) || std::fabs(lvPriceD) > kMaxEuropeanCallPriceAbs) {
                ++nSkipLvPrice;
                continue;
            }
            auto nodeVolTs = Handle<BlackVolTermStructure>(
                ext::make_shared<BlackConstantVol>(md.today(), md.calendar(), surfaceIv, md.dayCounter()));
            auto nodeBsProcess = ext::make_shared<GeneralizedBlackScholesProcess>(
                testSpot, xTs, xTs, nodeVolTs);
            option.setPricingEngine(ext::make_shared<AnalyticEuropeanEngine>(nodeBsProcess));
            const Real bsPrice = option.NPV();
            const double bsPriceD = benchmarkToDouble(bsPrice);
            if (!std::isfinite(bsPriceD) || std::fabs(bsPriceD) > kMaxEuropeanCallPriceAbs) {
                ++nSkipBsPrice;
                continue;
            }
            if (lvPriceD <= kMinEuropeanCallPrice || bsPriceD <= kMinEuropeanCallPrice) {
                ++nSkipTinyCall;
                continue;
            }
            const double errPx = std::fabs(benchmarkToDouble(lvPrice - bsPrice));
            const double errSpotBp = 10000.0 * errPx / kPriceScaleForSpotBpX;
            absErrPxValues.push_back(errPx);
            absErrSpotBpValues.push_back(errSpotBp);
            const double realIv = benchmarkToDouble(surfaceIv);
            double fittedIv = std::numeric_limits<double>::quiet_NaN();

            try {
                auto ivQuote = ext::make_shared<SimpleQuote>(realIv);
                auto ivVolTs = Handle<BlackVolTermStructure>(
                    ext::make_shared<BlackConstantVol>(
                        md.today(), md.calendar(), Handle<Quote>(ivQuote), md.dayCounter()));
                auto ivProcess = ext::make_shared<GeneralizedBlackScholesProcess>(
                    testSpot, xTs, xTs, ivVolTs);
                fittedIv = benchmarkToDouble(
                    option.impliedVolatility(lvPrice, ivProcess, 1.0e-8, 300, 1.0e-4, 4.0));
                const double absErrIvBp = 10000.0 * std::fabs(fittedIv - realIv);
                sumAbsErrIvBp += absErrIvBp;
                absErrIvBpValues.push_back(absErrIvBp);
                ++nIvPoints;
            } catch (...) {
            }
            sumAbsErrPx += errPx;
            sumAbsErrSpotBp += errSpotBp;
            ++nPoints;

            const double lvPriceS = paScaleS * lvPriceD;
            const Volatility sigmaMarketS = md.blackVolTs()->blackVol(expiry, strikeS, true);
            const Real forwardS = buehler.forward0T(expiry);
            const Real discountS = buehler.riskFreeTs()->discount(expiry);
            const Real stdDevS = sigmaMarketS * std::sqrt(t);
            const double refPriceS = benchmarkToDouble(buehlerMarketEuropeanCallPriceInS(md, buehler, expiry, strikeS));
            if (std::isfinite(refPriceS) && refPriceS > kMinEuropeanCallPrice &&
                std::fabs(refPriceS) <= kMaxEuropeanCallPriceAbs) {
                BuehlerLvBsSCompareRow row;
                row.expiry = expiry;
                row.strikeS = strikeS;
                row.lvPriceS = lvPriceS;
                row.refPriceS = refPriceS;
                row.absErrPxS = std::fabs(lvPriceS - refPriceS);
                buehlerSCompareRows.push_back(std::move(row));

                if (lvPriceS > kMinEuropeanCallPrice) {
                    try {
                        const Real stdDevImp = blackFormulaImpliedStdDev(
                            QuantLib::Option::Call, strikeS, forwardS, static_cast<Real>(lvPriceS),
                            discountS, 0.0, stdDevS, 1.0e-8, 200);
                        const double sigmaImpS = benchmarkToDouble(stdDevImp / std::sqrt(t));
                        const double sigmaMktS = benchmarkToDouble(sigmaMarketS);
                        if (std::isfinite(sigmaImpS) && sigmaImpS > 0.0 &&
                            std::isfinite(sigmaMktS) && sigmaMktS > 0.0) {
                            BuehlerSigmaSFromLvRow sRow;
                            sRow.expiry = expiry;
                            sRow.strikeS = strikeS;
                            sRow.sigmaMarketS = sigmaMktS;
                            sRow.sigmaImpliedS = sigmaImpS;
                            sRow.absErrIvBp = 10000.0 * std::fabs(sigmaImpS - sigmaMktS);
                            sigmaSFromLvRows.push_back(std::move(sRow));
                        }
                    } catch (...) {
                    }
                }
            }

            if (verbose) {
                std::cout << std::setw(16) << expiry
                          << " | " << std::setw(6) << strike
                          << " | " << std::setw(7) << lvPrice
                          << " | " << std::setw(7) << bsPrice
                          << " | " << std::setw(7) << realIv
                          << " | " << std::setw(7) << fittedIv << "\n";
            }
        }
    }
    const double meanAbsErrPx =
        (nPoints > 0) ? (sumAbsErrPx / static_cast<double>(nPoints)) : 0.0;
    const double meanAbsErrSpotBp =
        (nPoints > 0) ? (sumAbsErrSpotBp / static_cast<double>(nPoints)) : 0.0;
    const double meanAbsErrIvBp =
        (nIvPoints > 0) ? (sumAbsErrIvBp / static_cast<double>(nIvPoints)) : 0.0;
    const auto median = [](std::vector<double>& values) -> double {
        if (values.empty()) {
            return 0.0;
        }
        std::sort(values.begin(), values.end());
        const Size m = values.size();
        if (m % 2 == 0) {
            return 0.5 * (values[m / 2 - 1] + values[m / 2]);
        }
        return values[m / 2];
    };
    const double medianAbsErrPx = median(absErrPxValues);
    const double medianAbsErrSpotBp = median(absErrSpotBpValues);
    const double medianAbsErrIvBp = median(absErrIvBpValues);
    double maxAbsErrIvBp = 0.0;
    if (!absErrIvBpValues.empty()) {
        maxAbsErrIvBp = *std::max_element(absErrIvBpValues.begin(), absErrIvBpValues.end());
    }
    const Size nExcludedFromTable = nCandidatePairs - nPoints;
    if (verbose) {
        std::cout << "Verify coverage: candidates=" << nCandidatePairs << " printed=" << nPoints
                  << " excluded=" << nExcludedFromTable << " (plus " << earlyExpirySkip
                  << " expiry pillar(s) x " << md.strikes().size()
                  << " strikes never scanned)\n";
        std::cout << "  skip t<=0: " << nSkipT << " | A<=0: " << nSkipA
                  << " | kx<=0: " << nSkipKxNonPos << " | kx<kxTabLo: " << nSkipKxBelowTab
                  << " | bad LV price: " << nSkipLvPrice << " | bad BS price: " << nSkipBsPrice
                  << " | tiny call: " << nSkipTinyCall << " | FD failed: " << nSkipFdFailed
                  << "\n";
        if (nSkipKxBelowTab > 0) {
            std::cout << "  kx<kxTabLo strikes S (m=K/S0): ";
            bool first = true;
            for (Size i = strikeEdgePadding; i < md.strikes().size() - strikeEdgePadding; ++i) {
                const Real strikeS = md.strikes()[i];
                bool excluded = false;
                for (Size j = expiryStart; j < md.expiries().size() - expiryEdgePadding; ++j) {
                    const Date expiry = md.expiries()[j];
                    const Real D = buehler.dividendCarry0T(expiry);
                    const Real A = buehler.forward0T(expiry) - D;
                    if (A <= 0.0) {
                        continue;
                    }
                    const Real kx = (strikeS - D) / A;
                    if (kx > 0.0 && kx < kxTabLo - 1.0e-12) {
                        excluded = true;
                        break;
                    }
                }
                if (excluded) {
                    if (!first) {
                        std::cout << ", ";
                    }
                    first = false;
                    std::cout << strikeS << " (m=" << (strikeS / md.spotValue()) << ")";
                }
            }
            std::cout << "\n";
        }
        std::cout << "Mean abs err(px): " << meanAbsErrPx
                  << " | Mean abs err(spot bp): " << meanAbsErrSpotBp
                  << " | Median abs err(px): " << medianAbsErrPx
                  << " | Median abs err(spot bp): " << medianAbsErrSpotBp
                  << " | Mean abs err(iv bp): " << meanAbsErrIvBp
                  << " | Median abs err(iv bp): " << medianAbsErrIvBp
                  << " | Max abs err(iv bp): " << maxAbsErrIvBp << "\n";
    }

    if (verbose && !buehlerSCompareRows.empty()) {
        double sumAbsErrPxS = 0.0;
        double sumAbsErrSpotBpS = 0.0;
        std::vector<double> absErrPxSValues;
        std::vector<double> absErrSpotBpSValues;
        absErrPxSValues.reserve(buehlerSCompareRows.size());
        absErrSpotBpSValues.reserve(buehlerSCompareRows.size());
        for (const BuehlerLvBsSCompareRow& row : buehlerSCompareRows) {
            sumAbsErrPxS += row.absErrPxS;
            const double errSpotBpS = 10000.0 * row.absErrPxS / benchmarkToDouble(md.spotValue());
            sumAbsErrSpotBpS += errSpotBpS;
            absErrPxSValues.push_back(row.absErrPxS);
            absErrSpotBpSValues.push_back(errSpotBpS);
        }
        const Size nPointsS = buehlerSCompareRows.size();
        const double meanAbsErrPxS = sumAbsErrPxS / static_cast<double>(nPointsS);
        const double meanAbsErrSpotBpS = sumAbsErrSpotBpS / static_cast<double>(nPointsS);
        const double medianAbsErrPxS = median(absErrPxSValues);
        const double medianAbsErrSpotBpS = median(absErrSpotBpSValues);
        double maxAbsErrPxS = 0.0;
        if (!absErrPxSValues.empty()) {
            maxAbsErrPxS = *std::max_element(absErrPxSValues.begin(), absErrPxSValues.end());
        }

        std::cout << "\n=== Buehler round-trip in S ===\n";
        std::cout << "LVPrice_S = P(0,T)*A(T)*C_X^LV  |  RefPrice_S = P(0,T)[F_S N(d1)-K_S N(d2)], "
                     "sigma_S=market, F_S=forward0T(T)\n";
        std::cout << "Expiry           | Strike_S | LVPrice_S | RefPrice_S\n";
        std::cout << "---------------------------------------------------------\n";
        for (const BuehlerLvBsSCompareRow& row : buehlerSCompareRows) {
            std::cout << std::setw(16) << row.expiry << " | " << std::setw(6) << row.strikeS
                      << " | " << std::setw(9) << row.lvPriceS << " | " << std::setw(9)
                      << row.refPriceS << "\n";
        }
        std::cout << "Mean abs err(px) [S]: " << meanAbsErrPxS
                  << " | Mean abs err(spot bp) [S]: " << meanAbsErrSpotBpS
                  << " | Median abs err(px) [S]: " << medianAbsErrPxS
                  << " | Median abs err(spot bp) [S]: " << medianAbsErrSpotBpS
                  << " | Max abs err(px) [S]: " << maxAbsErrPxS << "\n";

    }

    if (!sigmaSFromLvRows.empty()) {
        double sumAbsErrIvBpS = 0.0;
        std::vector<double> absErrIvBpSValues;
        absErrIvBpSValues.reserve(sigmaSFromLvRows.size());
        for (const BuehlerSigmaSFromLvRow& row : sigmaSFromLvRows) {
            sumAbsErrIvBpS += row.absErrIvBp;
            absErrIvBpSValues.push_back(row.absErrIvBp);
        }
        const Size nSigmaS = sigmaSFromLvRows.size();
        const double meanAbsErrIvBpS = sumAbsErrIvBpS / static_cast<double>(nSigmaS);
        const double medianAbsErrIvBpS = median(absErrIvBpSValues);
        double maxAbsErrIvBpS = 0.0;
        if (!absErrIvBpSValues.empty()) {
            maxAbsErrIvBpS =
                *std::max_element(absErrIvBpSValues.begin(), absErrIvBpSValues.end());
        }

        std::cout << std::fixed << std::setprecision(6);
        if (verbose) {
            std::cout << "\n=== Implied sigma_S from LV round-trip (vol circle) ===\n";
            std::cout << "sigma_S^impl: invert blackFormula on LVPrice_S (F_S=forward0T, same as RefPrice_S)\n";
        } else {
            std::cout << "\n=== LV smile fit: sigma_S (market vs implied from FD) ===\n";
        }
        std::cout << "Expiry           | Strike_S | sigma_S mkt | sigma_S impl | err (bp)\n";
        std::cout << "--------------------------------------------------------------------\n";
        for (const BuehlerSigmaSFromLvRow& row : sigmaSFromLvRows) {
            std::cout << std::setw(16) << row.expiry << " | " << std::setw(8) << row.strikeS
                      << " | " << std::setw(11) << row.sigmaMarketS << " | " << std::setw(12)
                      << row.sigmaImpliedS << " | " << std::setw(8) << row.absErrIvBp << "\n";
        }
        if (verbose) {
            std::cout << "Mean abs err(iv bp) [sigma_S]: " << meanAbsErrIvBpS
                      << " | Median: " << medianAbsErrIvBpS << " | Max: " << maxAbsErrIvBpS << "\n";
        } else {
            std::cout << "Mean abs err(iv bp): " << meanAbsErrIvBpS << "\n";
        }

    }

    export_lv_fixed_x_csv(buehler, "build-std/lv_fixed_x.csv", verbose);
}

void verify_lsv_mc_vs_lv_fd(const MarketData& md,
                            BuehlerModel& buehler,
                            const QuantLib::Size nSubbanks,
                            const QuantLib::Size subbankSamples,
                            const QuantLib::Size tGridPerYear,
                            const QuantLib::Size xGrid,
                            const bool verbose) {
    ScopedTeeLog logCapture("out_lsv.txt");
    using namespace QuantLib;
    QL_REQUIRE(!md.expiries().empty(), "MarketData not initialized: expiries are empty");
    QL_REQUIRE(!md.strikes().empty(), "MarketData not initialized: strikes are empty");
    QL_REQUIRE(buehler.hasLsvCalibration(),
               "verify_lsv_mc_vs_lv_fd: Bergomi params not set on model");
    QL_REQUIRE(!buehler.fixedPureLocalVolTs().empty(),
               "verify_lsv_mc_vs_lv_fd: run BuehlerModel::calibration (fixed pure-X LV)");
    QL_REQUIRE(nSubbanks > 0, "verify_lsv_mc_vs_lv_fd: nSubbanks must be positive");
    QL_REQUIRE(subbankSamples > 0,
               "verify_lsv_mc_vs_lv_fd: subbankSamples must be positive");
    QL_REQUIRE(tGridPerYear > 0, "verify_lsv_mc_vs_lv_fd: tGridPerYear must be positive");
    QL_REQUIRE(xGrid > 0, "verify_lsv_mc_vs_lv_fd: xGrid must be positive");

    const Size strikeEdgePadding = 0;
    const Size expiryEdgePadding = 0;
    constexpr double kMinEuropeanCallPrice = 1.0e-9;
    constexpr double kMinVerifyFdPriceS = 1.0;
    constexpr double kMaxEuropeanCallPriceAbs = 1.0e8;
    const double spotS0 = benchmarkToDouble(md.spotValue());
    const Real kxTabLo =
        buehler.denseXStrikes().empty() ? 0.0 : buehler.denseXStrikes().front();
    const Size earlyExpirySkip = 1;
    const Size expiryStart = expiryEdgePadding + earlyExpirySkip;
    Size nSkipFdFailed = 0;

    QL_REQUIRE(md.strikes().size() > 2 * strikeEdgePadding,
               "Not enough strikes for verify grid");
    QL_REQUIRE(md.expiries().size() > 2 * expiryEdgePadding,
               "Not enough expiries for verify grid");
    QL_REQUIRE(expiryStart < md.expiries().size() - expiryEdgePadding,
               "Not enough expiries after skipping early maturities");

    const auto impliedSigmaSFromCallPrice = [](const Real strikeS, const Real forwardS,
                                              const Real priceS, const Real discountS,
                                              const Time t, const Real sigmaGuessS) -> Real {
        const Real stdDevGuess = sigmaGuessS * std::sqrt(t);
        const Real stdDevImp = blackFormulaImpliedStdDev(
            QuantLib::Option::Call, strikeS, forwardS, priceS, discountS, 0.0, stdDevGuess, 1.0e-8,
            200);
        return stdDevImp / std::sqrt(t);
    };
    const auto tryImpliedSigmaS = [&](const Real strikeS, const Real forwardS, const Real priceS,
                                      const Real discountS, const Time t,
                                      const Real sigmaGuessS) -> Real {
        if (!(priceS > kMinEuropeanCallPrice)) {
            return Null<Real>();
        }
        try {
            const Real sigmaImp =
                impliedSigmaSFromCallPrice(strikeS, forwardS, priceS, discountS, t, sigmaGuessS);
            if (std::isfinite(sigmaImp) && sigmaImp > 0.0) {
                return sigmaImp;
            }
        } catch (...) {
        }
        return Null<Real>();
    };

    struct ScenarioRow {
        Date expiry;
        Real strikeS = 0.0;
        Real kx = 0.0;
        Time t = 0.0;
        Real forwardS = 0.0;
        Real discountS = 0.0;
        Real scaleS = 0.0;
        Real lvPriceS = 0.0;
        Real ivLvS = Null<Real>();
    };
    std::vector<ScenarioRow> scenarios;

    for (Size j = expiryStart; j < md.expiries().size() - expiryEdgePadding; ++j) {
        const Date expiry = md.expiries()[j];
        const Time t = md.dayCounter().yearFraction(md.today(), expiry);
        if (t <= 0.0) {
            continue;
        }

        for (Size i = strikeEdgePadding; i < md.strikes().size() - strikeEdgePadding; ++i) {
            const Real strikeS = md.strikes()[i];
            const Real kx = LvEuropeanFdBuehlerOption::pureXStrikeFromSpot(buehler, expiry, strikeS);
            if (kx <= 0.0) {
                continue;
            }
            if (!buehler.denseXStrikes().empty() && kx < kxTabLo - 1.0e-12) {
                continue;
            }

            OptionContractParams fdParams;
            fdParams.expiry = expiry;
            fdParams.strike = strikeS;
            fdParams.isCall = true;
            Real lvPriceS = Null<Real>();
            try {
                lvPriceS =
                    LvEuropeanFdBuehlerOption(fdParams, BuehlerOptionPriceSpace::S, tGridPerYear,
                                              xGrid)
                        .price(buehler);
            } catch (const std::exception&) {
                ++nSkipFdFailed;
                continue;
            }
            const double lvPriceSD = benchmarkToDouble(lvPriceS);
            if (!std::isfinite(lvPriceSD) || std::fabs(lvPriceSD) > kMaxEuropeanCallPriceAbs) {
                continue;
            }
            if (lvPriceSD < kMinVerifyFdPriceS) {
                continue;
            }

            const Real D = buehler.dividendCarry0T(expiry);
            const Real A = buehler.forward0T(expiry) - D;
            if (A <= 0.0) {
                continue;
            }
            const Real forwardS = buehler.forward0T(expiry);
            const Real discountS = buehler.riskFreeTs()->discount(expiry);
            const Real scaleS = discountS * A;
            const Volatility sigmaSeedS = md.blackVolTs()->blackVol(expiry, strikeS, true);
            const Real ivLvS = tryImpliedSigmaS(strikeS, forwardS, lvPriceS, discountS, t, sigmaSeedS);
            ScenarioRow scenario;
            scenario.expiry = expiry;
            scenario.strikeS = strikeS;
            scenario.kx = kx;
            scenario.t = t;
            scenario.forwardS = forwardS;
            scenario.discountS = discountS;
            scenario.scaleS = scaleS;
            scenario.lvPriceS = lvPriceS;
            scenario.ivLvS = ivLvS;
            scenarios.push_back(scenario);
        }
    }

    if (scenarios.empty()) {
        std::cout << "verify_lsv_mc_vs_lv_fd: no scenarios (FD failed: " << nSkipFdFailed
                  << ")\n";
        return;
    }

    const Date horizonMax = md.expiries().back();
    BuehlerMcSettings mcSettings;
    mcSettings.mcSamples = subbankSamples;
    mcSettings.dynamics = BuehlerMcDynamics::Lsv;
    mcSettings.lsvBins = kDefaultLsvBins;
    mcSettings.mcPathWorkers = buehlerMcPathWorkersFromEnvironment();
    mcSettings.mcSavePathFixingDates = md.expiries();

    if (verbose) {
        std::cout << "\n=== LSV MC vs LV FD in S (verify grid) ===\n"
                  << "Today: " << md.today() << '\n'
                  << "LVPrice = LvEuropeanFdBuehlerOption in S (same FD as verify_LV_BS_consistency)\n"
                  << "IV_LV / IV_LSV = sigma_S implied from LVPrice / LSVPrice (Black inversion)\n"
                  << "LSVPrice = mean over " << nSubbanks << " independent sub-banks x " << subbankSamples
                  << " paths each (" << (nSubbanks * subbankSamples) << " total simulated paths)\n"
                  << "MC evolve: daily business days to " << horizonMax
                  << "; bank stores " << mcSettings.mcSavePathFixingDates.size()
                  << " maturity fixings (mcSavePathFixingDates)\n"
                  << "lsvBins=" << mcSettings.lsvBins << " (adaptive cloud min/max per step)"
                  << ", pathsPerBin~"
                  << (mcSettings.lsvBins > 0 ? subbankSamples / mcSettings.lsvBins : 0)
                  << ", skip earliest " << earlyExpirySkip << " expiry pillar(s), kxTabLo=" << kxTabLo
                  << "\nFD grid: " << tGridPerYear << " t-steps/year x " << xGrid
                  << " x-nodes; drop rows with LVPrice (FD in S) < " << kMinVerifyFdPriceS
                  << "; FD failed: " << nSkipFdFailed << '\n'
                  << "Scenarios: " << scenarios.size() << '\n'
                  << std::flush;
    } else {
        std::cout << "\n=== LSV MC vs LV FD ===\n" << std::flush;
    }

    std::vector<long double> sumLsvPriceX(scenarios.size(), 0.0L);
    std::vector<Size> subbankCount(scenarios.size(), 0);

    for (Size subbank = 0; subbank < nSubbanks; ++subbank) {
        mcSettings.seed = kDefaultMcSeed + static_cast<BigNatural>(subbank) * 1000003ULL;
        if (verbose) {
            std::cout << "Sub-bank " << (subbank + 1) << '/' << nSubbanks << " (seed=" << mcSettings.seed
                      << ", mcSamples=" << subbankSamples << ") ...\n"
                      << std::flush;
        }
        buehler.simulateFixingPaths(horizonMax, {}, mcSettings);
        const BuehlerFixingSavePath& bank = buehler.fixingSavePath();

        for (Size s = 0; s < scenarios.size(); ++s) {
            const ScenarioRow& scenario = scenarios[s];
            QL_REQUIRE(bank.hasFixingDate(scenario.expiry),
                       "verify_lsv_mc_vs_lv_fd: expiry " << scenario.expiry
                                                                      << " not on LSV bank");
            OptionContractParams mcParams;
            mcParams.expiry = scenario.expiry;
            mcParams.strike = scenario.kx;
            mcParams.isCall = true;
            const BuehlerMcPathPricingResult mc =
                EuropeanMcBuehlerOption::priceFromSavePath(
                    bank, mcParams, buehler, BuehlerOptionPriceSpace::X);
            sumLsvPriceX[s] += static_cast<long double>(mc.value);
            ++subbankCount[s];
        }
    }

    struct ResultRow {
        ScenarioRow scenario;
        Real lsvPriceS = 0.0;
        Real absErrPx = 0.0;
        Real absErrSpotBp = 0.0;
        Real ivLsvS = Null<Real>();
        bool inIvTable = false;
        double absErrIvBp = 0.0;
    };
    std::vector<ResultRow> results;
    results.reserve(scenarios.size());

    for (Size s = 0; s < scenarios.size(); ++s) {
        QL_REQUIRE(subbankCount[s] == nSubbanks,
                   "verify_lsv_mc_vs_lv_fd: incomplete sub-bank coverage");
        const ScenarioRow& scenario = scenarios[s];
        const Real lsvPriceX =
            static_cast<Real>(sumLsvPriceX[s] / static_cast<long double>(nSubbanks));

        ResultRow row;
        row.scenario = scenario;
        row.lsvPriceS = scenario.scaleS * lsvPriceX;

        row.absErrPx = std::fabs(row.lsvPriceS - scenario.lvPriceS);
        row.absErrSpotBp = 10000.0 * row.absErrPx / spotS0;

        const Real ivGuessForLsv =
            scenario.ivLvS != Null<Real>() ? scenario.ivLvS : 0.2;
        row.ivLsvS = tryImpliedSigmaS(scenario.strikeS, scenario.forwardS, row.lsvPriceS,
                                      scenario.discountS, scenario.t, ivGuessForLsv);
        if (scenario.ivLvS != Null<Real>() && row.ivLsvS != Null<Real>()) {
            row.inIvTable = true;
            row.absErrIvBp =
                10000.0 * std::fabs(benchmarkToDouble(row.ivLsvS) - benchmarkToDouble(scenario.ivLvS));
        }
        results.push_back(row);
    }

    QL_REQUIRE(!results.empty(), "verify_lsv_mc_vs_lv_fd: no rows after MC pricing");

    std::cout << std::fixed << std::setprecision(6);
    std::cout << "Expiry           | Strike | LVPrice | LSVPrice | IV_LV | IV_LSV\n";
    std::cout << "-----------------------------------------------------------------\n";

    std::vector<double> absErrPxValues;
    std::vector<double> absErrSpotBpValues;
    std::vector<double> absErrIvBpValues;
    absErrPxValues.reserve(results.size());
    absErrSpotBpValues.reserve(results.size());

    for (const ResultRow& row : results) {
        absErrPxValues.push_back(row.absErrPx);
        absErrSpotBpValues.push_back(row.absErrSpotBp);
        std::cout << std::setw(16) << row.scenario.expiry << " | " << std::setw(6)
                  << row.scenario.strikeS << " | " << std::setw(7) << row.scenario.lvPriceS
                  << " | " << std::setw(8) << row.lsvPriceS << " | ";
        if (row.scenario.ivLvS == Null<Real>()) {
            std::cout << "      - | ";
        } else {
            std::cout << std::setw(7) << row.scenario.ivLvS << " | ";
        }
        if (row.inIvTable) {
            std::cout << std::setw(7) << row.ivLsvS << '\n';
            absErrIvBpValues.push_back(row.absErrIvBp);
        } else {
            std::cout << "      -\n";
        }
    }

    double sumAbsErrPx = 0.0;
    double sumAbsErrSpotBp = 0.0;
    double sumAbsErrIvBp = 0.0;
    for (const ResultRow& row : results) {
        sumAbsErrPx += row.absErrPx;
        sumAbsErrSpotBp += row.absErrSpotBp;
        if (row.inIvTable) {
            sumAbsErrIvBp += row.absErrIvBp;
        }
    }
    const double meanAbsErrPx = sumAbsErrPx / static_cast<double>(results.size());
    const double meanAbsErrSpotBp = sumAbsErrSpotBp / static_cast<double>(results.size());
    const auto median = [](std::vector<double> values) -> double {
        if (values.empty()) {
            return 0.0;
        }
        std::sort(values.begin(), values.end());
        const Size m = values.size();
        if (m % 2 == 0) {
            return 0.5 * (values[m / 2 - 1] + values[m / 2]);
        }
        return values[m / 2];
    };
    const double medianAbsErrPx = median(absErrPxValues);
    const double medianAbsErrSpotBp = median(absErrSpotBpValues);
    const Size nIvPoints = absErrIvBpValues.size();
    const double meanAbsErrIvBp =
        nIvPoints > 0 ? (sumAbsErrIvBp / static_cast<double>(nIvPoints)) : 0.0;
    const double medianAbsErrIvBp = median(absErrIvBpValues);
    double maxAbsErrIvBp = 0.0;
    if (nIvPoints > 0) {
        maxAbsErrIvBp = *std::max_element(absErrIvBpValues.begin(), absErrIvBpValues.end());
    }

    if (verbose) {
        std::cout << "Mean abs err(px): " << meanAbsErrPx
                  << " | Mean abs err(spot bp): " << meanAbsErrSpotBp
                  << " | Median abs err(px): " << medianAbsErrPx
                  << " | Median abs err(spot bp): " << medianAbsErrSpotBp
                  << " | Mean abs err(iv bp): " << meanAbsErrIvBp << " (n=" << nIvPoints << ")"
                  << " | Median abs err(iv bp): " << medianAbsErrIvBp
                  << " | Max abs err(iv bp): " << maxAbsErrIvBp << '\n';
    } else {
        std::cout << "Mean abs err(px): " << meanAbsErrPx
                  << " | Mean abs err(iv bp): " << meanAbsErrIvBp << '\n';
    }
}

std::vector<LvIvFitRow> collect_lv_iv_fit_grid(const MarketData& md,
                                               const BuehlerModel& buehler,
                                               const QuantLib::Size tGridPerYear,
                                               const QuantLib::Size xGrid) {
    using namespace QuantLib;
    QL_REQUIRE(!md.expiries().empty(), "collect_lv_iv_fit_grid: empty expiries");
    QL_REQUIRE(!md.strikes().empty(), "collect_lv_iv_fit_grid: empty strikes");
    QL_REQUIRE(!buehler.fixedPureLocalVolTs().empty(),
               "collect_lv_iv_fit_grid: run BuehlerModel::calibration first");
    QL_REQUIRE(tGridPerYear > 0, "collect_lv_iv_fit_grid: tGridPerYear must be positive");
    QL_REQUIRE(xGrid > 0, "collect_lv_iv_fit_grid: xGrid must be positive");

    constexpr double kMinEuropeanCallPrice = 1.0e-9;
    const Size earlyExpirySkip = 1;
    const Size expiryStart = earlyExpirySkip;
    const Real kxTabLo =
        buehler.denseXStrikes().empty() ? 0.0 : buehler.denseXStrikes().front();

    std::vector<LvIvFitRow> rows;
    for (Size j = expiryStart; j < md.expiries().size(); ++j) {
        const Date expiry = md.expiries()[j];
        const Time t = md.dayCounter().yearFraction(md.today(), expiry);
        if (t <= 0.0) {
            continue;
        }
        const Real forwardS = buehler.forward0T(expiry);
        const Real discountS = buehler.riskFreeTs()->discount(expiry);
        const Volatility sigmaSeedS = md.blackVolTs()->blackVol(expiry, md.strikes().front(), true);

        for (Size i = 0; i < md.strikes().size(); ++i) {
            const Real strikeS = md.strikes()[i];
            const Real D = buehler.dividendCarry0T(expiry);
            const Real A = buehler.forward0T(expiry) - D;
            if (A <= 0.0) {
                continue;
            }
            const Real kx = (strikeS - D) / A;
            if (kx <= 0.0) {
                continue;
            }
            if (!buehler.denseXStrikes().empty() && kx < kxTabLo - 1.0e-12) {
                continue;
            }

            OptionContractParams fdParams;
            fdParams.expiry = expiry;
            fdParams.strike = strikeS;
            fdParams.isCall = true;
            Real lvPriceS = Null<Real>();
            try {
                lvPriceS =
                    LvEuropeanFdBuehlerOption(fdParams, BuehlerOptionPriceSpace::S, tGridPerYear,
                                              xGrid)
                        .price(buehler);
            } catch (const std::exception&) {
                continue;
            }
            if (!(lvPriceS > kMinEuropeanCallPrice)) {
                continue;
            }

            const Volatility sigmaMarketS = md.blackVolTs()->blackVol(expiry, strikeS, true);
            try {
                const Real stdDevImp = blackFormulaImpliedStdDev(
                    QuantLib::Option::Call, strikeS, forwardS, lvPriceS, discountS, 0.0,
                    sigmaSeedS * std::sqrt(t), 1.0e-8, 200);
                const double sigmaLvS = benchmarkToDouble(stdDevImp / std::sqrt(t));
                const double sigmaMktS = benchmarkToDouble(sigmaMarketS);
                if (std::isfinite(sigmaLvS) && sigmaLvS > 0.0 && std::isfinite(sigmaMktS) &&
                    sigmaMktS > 0.0) {
                    LvIvFitRow row;
                    row.expiry = expiry;
                    row.strikeS = strikeS;
                    row.tenorYears = t;
                    row.sigmaMarketS = sigmaMktS;
                    row.sigmaLvS = sigmaLvS;
                    row.absErrIvBp = 10000.0 * std::fabs(sigmaLvS - sigmaMktS);
                    rows.push_back(std::move(row));
                }
            } catch (const std::exception&) {
            }
        }
    }
    return rows;
}

std::vector<LsvVsLvRow> collect_lsv_vs_lv_grid(const MarketData& md,
                                               BuehlerModel& buehler,
                                               const QuantLib::Size nSubbanks,
                                               const QuantLib::Size subbankSamples,
                                               const QuantLib::Size tGridPerYear,
                                               const QuantLib::Size xGrid) {
    using namespace QuantLib;
    QL_REQUIRE(!md.expiries().empty(), "collect_lsv_vs_lv_grid: empty expiries");
    QL_REQUIRE(!md.strikes().empty(), "collect_lsv_vs_lv_grid: empty strikes");
    QL_REQUIRE(buehler.hasLsvCalibration(),
               "collect_lsv_vs_lv_grid: Bergomi params not set on model");
    QL_REQUIRE(!buehler.fixedPureLocalVolTs().empty(),
               "collect_lsv_vs_lv_grid: run BuehlerModel::calibration first");
    QL_REQUIRE(nSubbanks > 0, "collect_lsv_vs_lv_grid: nSubbanks must be positive");
    QL_REQUIRE(subbankSamples > 0, "collect_lsv_vs_lv_grid: subbankSamples must be positive");
    QL_REQUIRE(tGridPerYear > 0, "collect_lsv_vs_lv_grid: tGridPerYear must be positive");
    QL_REQUIRE(xGrid > 0, "collect_lsv_vs_lv_grid: xGrid must be positive");

    const Size strikeEdgePadding = 0;
    const Size expiryEdgePadding = 0;
    constexpr double kMinEuropeanCallPrice = 1.0e-9;
    constexpr double kMinVerifyFdPriceS = 1.0;
    const double spotS0 = benchmarkToDouble(md.spotValue());
    const Real kxTabLo =
        buehler.denseXStrikes().empty() ? 0.0 : buehler.denseXStrikes().front();
    const Size earlyExpirySkip = 1;
    const Size expiryStart = expiryEdgePadding + earlyExpirySkip;
    Size nSkipFdFailed = 0;

    const auto tryImpliedSigmaS = [](const Real strikeS, const Real forwardS, const Real priceS,
                                      const Real discountS, const Time t,
                                      const Real sigmaGuessS) -> Real {
        if (!(priceS > kMinEuropeanCallPrice)) {
            return Null<Real>();
        }
        try {
            const Real stdDevImp = blackFormulaImpliedStdDev(
                QuantLib::Option::Call, strikeS, forwardS, priceS, discountS, 0.0, sigmaGuessS,
                1.0e-8, 200);
            const Real sigmaImp = stdDevImp / std::sqrt(t);
            if (std::isfinite(sigmaImp) && sigmaImp > 0.0) {
                return sigmaImp;
            }
        } catch (...) {
        }
        return Null<Real>();
    };

    struct ScenarioRow {
        Date expiry;
        Real strikeS = 0.0;
        Real kx = 0.0;
        Time t = 0.0;
        Real forwardS = 0.0;
        Real discountS = 0.0;
        Real scaleS = 0.0;
        Real lvPriceS = 0.0;
        Real ivLvS = Null<Real>();
    };
    std::vector<ScenarioRow> scenarios;

    for (Size j = expiryStart; j < md.expiries().size() - expiryEdgePadding; ++j) {
        const Date expiry = md.expiries()[j];
        const Time t = md.dayCounter().yearFraction(md.today(), expiry);
        if (t <= 0.0) {
            continue;
        }

        for (Size i = strikeEdgePadding; i < md.strikes().size() - strikeEdgePadding; ++i) {
            const Real strikeS = md.strikes()[i];
            const Real kx = LvEuropeanFdBuehlerOption::pureXStrikeFromSpot(buehler, expiry, strikeS);
            if (kx <= 0.0) {
                continue;
            }
            if (!buehler.denseXStrikes().empty() && kx < kxTabLo - 1.0e-12) {
                continue;
            }

            OptionContractParams fdParams;
            fdParams.expiry = expiry;
            fdParams.strike = strikeS;
            fdParams.isCall = true;
            Real lvPriceS = Null<Real>();
            try {
                lvPriceS =
                    LvEuropeanFdBuehlerOption(fdParams, BuehlerOptionPriceSpace::S, tGridPerYear,
                                              xGrid)
                        .price(buehler);
            } catch (const std::exception&) {
                ++nSkipFdFailed;
                continue;
            }
            if (!(lvPriceS > kMinVerifyFdPriceS)) {
                continue;
            }

            const Real forwardS = buehler.forward0T(expiry);
            const Real discountS = buehler.riskFreeTs()->discount(expiry);
            const Real scaleS = discountS * (forwardS - buehler.dividendCarry0T(expiry));
            const Volatility sigmaSeedS = md.blackVolTs()->blackVol(expiry, strikeS, true);
            const Real ivLvS =
                tryImpliedSigmaS(strikeS, forwardS, lvPriceS, discountS, t, sigmaSeedS);
            ScenarioRow scenario;
            scenario.expiry = expiry;
            scenario.strikeS = strikeS;
            scenario.kx = kx;
            scenario.t = t;
            scenario.forwardS = forwardS;
            scenario.discountS = discountS;
            scenario.scaleS = scaleS;
            scenario.lvPriceS = lvPriceS;
            scenario.ivLvS = ivLvS;
            scenarios.push_back(scenario);
        }
    }

    QL_REQUIRE(!scenarios.empty(),
               "collect_lsv_vs_lv_grid: no scenarios (FD failed: " << nSkipFdFailed << ")");

    const Date horizonMax = md.expiries().back();
    BuehlerMcSettings mcSettings;
    mcSettings.mcSamples = subbankSamples;
    mcSettings.dynamics = BuehlerMcDynamics::Lsv;
    mcSettings.lsvBins = kDefaultLsvBins;
    mcSettings.mcPathWorkers = buehlerMcPathWorkersFromEnvironment();
    mcSettings.mcSavePathFixingDates = md.expiries();

    std::vector<long double> sumLsvPriceX(scenarios.size(), 0.0L);
    std::vector<Size> subbankCount(scenarios.size(), 0);

    for (Size subbank = 0; subbank < nSubbanks; ++subbank) {
        mcSettings.seed = kDefaultMcSeed + static_cast<BigNatural>(subbank) * 1000003ULL;
        buehler.simulateFixingPaths(horizonMax, {}, mcSettings);
        const BuehlerFixingSavePath& bank = buehler.fixingSavePath();

        for (Size s = 0; s < scenarios.size(); ++s) {
            const ScenarioRow& scenario = scenarios[s];
            QL_REQUIRE(bank.hasFixingDate(scenario.expiry),
                       "collect_lsv_vs_lv_grid: expiry " << scenario.expiry << " not on LSV bank");
            OptionContractParams mcParams;
            mcParams.expiry = scenario.expiry;
            mcParams.strike = scenario.kx;
            mcParams.isCall = true;
            const BuehlerMcPathPricingResult mc = EuropeanMcBuehlerOption::priceFromSavePath(
                bank, mcParams, buehler, BuehlerOptionPriceSpace::X);
            sumLsvPriceX[s] += static_cast<long double>(mc.value);
            ++subbankCount[s];
        }
    }

    std::vector<LsvVsLvRow> results;
    results.reserve(scenarios.size());
    for (Size s = 0; s < scenarios.size(); ++s) {
        QL_REQUIRE(subbankCount[s] == nSubbanks,
                   "collect_lsv_vs_lv_grid: incomplete sub-bank coverage");
        const ScenarioRow& scenario = scenarios[s];
        const Real lsvPriceX =
            static_cast<Real>(sumLsvPriceX[s] / static_cast<long double>(nSubbanks));
        const Real lsvPriceS = scenario.scaleS * lsvPriceX;

        LsvVsLvRow row;
        row.expiry = scenario.expiry;
        row.strikeS = scenario.strikeS;
        row.lvPriceS = scenario.lvPriceS;
        row.lsvPriceS = lsvPriceS;
        row.ivLvS = scenario.ivLvS != Null<Real>() ? benchmarkToDouble(scenario.ivLvS)
                                                   : std::numeric_limits<double>::quiet_NaN();
        const Real ivGuessForLsv =
            scenario.ivLvS != Null<Real>() ? scenario.ivLvS : static_cast<Real>(0.2);
        const Real ivLsv = tryImpliedSigmaS(scenario.strikeS, scenario.forwardS, lsvPriceS,
                                            scenario.discountS, scenario.t, ivGuessForLsv);
        row.ivLsvS = ivLsv != Null<Real>() ? benchmarkToDouble(ivLsv)
                                           : std::numeric_limits<double>::quiet_NaN();
        if (scenario.ivLvS != Null<Real>() && ivLsv != Null<Real>()) {
            row.absErrIvBp =
                10000.0 * std::fabs(benchmarkToDouble(ivLsv) - benchmarkToDouble(scenario.ivLvS));
        } else {
            row.absErrIvBp = std::numeric_limits<double>::quiet_NaN();
        }
        results.push_back(std::move(row));
    }
    return results;
}
