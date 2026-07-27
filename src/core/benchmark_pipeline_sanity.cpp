/**
 * @file benchmark_pipeline_sanity.cpp
 */

#include "benchmark.h"
#include "asian_mc_buehler_option.h"
#include "autocall_mc_buehler_option.h"
#include "barrier_mc_buehler_option.h"
#include "lookback_mc_buehler_option.h"
#include "mc_observation_schedule.h"
#include "benchmark_bs_flat_reference.h"
#include "bs_flat_mc_save_path.h"
#include "buehler_fixing_path_simulate.h"
#include "buehler_mc_path_pricing.h"
#include "buehler_model.h"
#include "lv_digital_accrual_fd_buehler_option.h"
#include "lv_digital_fd_buehler_option.h"
#include "lv_european_fd_buehler_option.h"
#include "market_data.h"
#include <iomanip>
#include <iostream>
#include <ql/quantlib.hpp>

namespace {

struct AsianScheduleAccumulators {
    McSubbankAccumulator buehlerGeomFixed;
    McSubbankAccumulator buehlerGeomFloating;
    McSubbankAccumulator buehlerArithFixed;
    McSubbankAccumulator buehlerArithFloating;
    McSubbankAccumulator bsGeomFloating;
    McSubbankAccumulator bsArithFixedMc;
    McSubbankAccumulator bsArithFloating;
};

struct BarrierScheduleAccumulators {
    McSubbankAccumulator buehlerDownOut;
    McSubbankAccumulator buehlerDownIn;
    McSubbankAccumulator buehlerUpOut;
    McSubbankAccumulator buehlerUpIn;
    McSubbankAccumulator bsDownOut;
    McSubbankAccumulator bsDownIn;
    McSubbankAccumulator bsUpOut;
    McSubbankAccumulator bsUpIn;
};

struct LookbackScheduleAccumulators {
    McSubbankAccumulator buehlerFixed;
    McSubbankAccumulator buehlerFloating;
    McSubbankAccumulator bsFixed;
    McSubbankAccumulator bsFloating;
};

struct AutocallScheduleAccumulators {
    McSubbankAccumulator buehlerAutocall;
    McSubbankAccumulator bsAutocall;
};

struct TenorAccumulators {
    AsianScheduleAccumulators dailyAsian;
    AsianScheduleAccumulators monthlyAsian;
    BarrierScheduleAccumulators dailyBarrier;
    BarrierScheduleAccumulators monthlyBarrier;
    LookbackScheduleAccumulators dailyLookback;
    LookbackScheduleAccumulators monthlyLookback;
    AutocallScheduleAccumulators monthlyAutocallPhoenix;
    AutocallScheduleAccumulators monthlyAutocallAthena;
    AutocallScheduleAccumulators annualAutocallPhoenix;
    AutocallScheduleAccumulators annualAutocallAthena;
    std::vector<QuantLib::Date> dailyFixings;
    std::vector<QuantLib::Date> monthlyFixings;
    std::vector<QuantLib::Date> annualFixings;
    QuantLib::Date expiry;
    QuantLib::Real strikeF = 0.0;
    QuantLib::Real spotS = 0.0;
};

} // namespace

/** @copydoc pipeline_sanity_check_BS_fallback */
double pipeline_sanity_check_BS_fallback(const QuantLib::BigNatural buehlerMcSeed,
                                         const QuantLib::BigNatural bsSeed,
                                         const QuantLib::Size nSubbanks,
                                         const QuantLib::Size subbankSamples) {
    MarketData md;
    md.loadConstantMock();

    // The flat-BS reference pricers below use QuantLib Instruments (European /
    // discrete Asian engines) which read the global evaluation date. The library's
    // own pricing path never touches it, so pin it here (RAII-restored) for the
    // duration of the benchmark.
    const QuantLib::SavedSettings savedQlSettings;
    QuantLib::Settings::instance().evaluationDate() = md.today();

    BuehlerModel buehler(md);
    buehler.preprocessing();
    buehler.calibration(/*runValidation=*/false);

    using namespace QuantLib;
    using namespace bs_flat_reference;

    QL_REQUIRE(nSubbanks > 0, "pipeline_sanity_check_BS_fallback: nSubbanks must be positive");
    QL_REQUIRE(subbankSamples > 0,
               "pipeline_sanity_check_BS_fallback: subbankSamples must be positive");

    constexpr Size kFdProductsPerTenor = 4;
    constexpr Size kAsianProductsPerTenor = 4;
    constexpr Size kBarrierProductsPerTenor = 4;
    constexpr Size kLookbackProductsPerTenor = 2;
    constexpr Size kAutocallProductsPerTenor = 2;
    constexpr Size kAsianSchedulesPerTenor = 2;
    constexpr Size kBarrierSchedulesPerTenor = 2;
    constexpr Size kLookbackSchedulesPerTenor = 2;
    constexpr Size kAutocallSchedulesPerTenor = 2;
    const Size nTenors = kAsianSanityTenorMonths.size();
    const Size nFdRows = nTenors * kFdProductsPerTenor;
    const Size nAsianRows = nTenors * kAsianProductsPerTenor * kAsianSchedulesPerTenor;
    const Size nBarrierRows = nTenors * kBarrierProductsPerTenor * kBarrierSchedulesPerTenor;
    const Size nLookbackRows = nTenors * kLookbackProductsPerTenor * kLookbackSchedulesPerTenor;
    const Size nAutocallRows = nTenors * kAutocallProductsPerTenor * kAutocallSchedulesPerTenor;
    double sumAbsErrFd = 0.0;
    double sumAbsErrAsian = 0.0;
    double sumAbsErrBarrier = 0.0;
    double sumAbsErrLookback = 0.0;
    double sumAbsErrAutocall = 0.0;

    const Date horizonMax =
        md.calendar().advance(md.today(), kAsianSavePathTenorMonths, Months, Following);
    const std::vector<Date> bankFixingDates =
        buehlerMcSimulationDatesEveryNBusinessDays(buehler, horizonMax, kDefaultMcBusinessDayStep);

    std::vector<TenorAccumulators> tenorAcc(nTenors);
    for (Size t = 0; t < nTenors; ++t) {
        const int months = kAsianSanityTenorMonths[t];
        tenorAcc[t].expiry = md.calendar().advance(md.today(), months, Months, Following);
        tenorAcc[t].strikeF = qlEquityForward(md, tenorAcc[t].expiry);
        tenorAcc[t].spotS = md.spotValue();
    }

    std::cout << std::fixed << std::setprecision(8);
    std::cout << "\n=== pipeline_sanity_check_BS_fallback (call, K=F) ===\n";
    gBsSeed = bsSeed;
    gPipelineSanityMcSamples = subbankSamples;
    printSanityLegend(buehlerMcSeed, bsSeed, nSubbanks, subbankSamples);

    for (Size subbank = 0; subbank < nSubbanks; ++subbank) {
        const BigNatural bsSeedSub =
            bsSeed + static_cast<BigNatural>(subbank) * kSubbankSeedStride;
        const BigNatural buehlerSeedSub =
            buehlerMcSeed + static_cast<BigNatural>(subbank) * kSubbankSeedStride;

        std::cout << ">> sub-bank " << (subbank + 1) << '/' << nSubbanks << " (BS seed="
                  << bsSeedSub << ", LV seed=" << buehlerSeedSub << ", mcSamples="
                  << subbankSamples << ") ...\n"
                  << std::flush;

        const BsFlatMcSavePath bsBank = simulateBsFlatMcSavePath(
            md, horizonMax, bankFixingDates, subbankSamples, bsSeedSub);

        BuehlerMcSettings mcSettings;
        mcSettings.seed = buehlerSeedSub;
        mcSettings.mcSamples = subbankSamples;
        mcSettings.dynamics = BuehlerMcDynamics::Lv;
        buehler.simulateFixingPaths(horizonMax, {}, mcSettings);
        QL_REQUIRE(buehler.hasFixingSavePath(),
                   "sanity: simulateFixingPaths did not store a save path");
        const std::vector<Date>& bankFixings = buehler.fixingPathSimulationDates();
        const BuehlerFixingSavePath& lvBank = buehler.fixingSavePath();

        for (Size t = 0; t < nTenors; ++t) {
            TenorAccumulators& acc = tenorAcc[t];
            acc.dailyFixings = bankFixingsThroughExpiry(bankFixings, acc.expiry);
            acc.monthlyFixings = bankFixingsLastDatePerMonth(bankFixings, acc.expiry);
            acc.annualFixings = bankFixingsLastDatePerYear(bankFixings, acc.expiry);

            const auto accumulateAsian =
                [&](const std::vector<Date>& fixings, AsianScheduleAccumulators& asianAcc) {
                    OptionContractParams asianP;
                    asianP.expiry = acc.expiry;
                    asianP.strike = acc.strikeF;
                    asianP.isCall = true;
                    asianP.observationDates = fixings;

                    const AsianMcFourPayoffs lvAsian =
                        AsianMcBuehlerOption::priceAllPayoffsFromSavePath(lvBank, asianP, buehler);
                    const AsianMcFourPayoffs bsAsian =
                        priceBsAsianAllPayoffsFromSavePath(bsBank, asianP, md);

                    asianAcc.buehlerGeomFixed.add(lvAsian.geometricFixed);
                    asianAcc.buehlerGeomFloating.add(lvAsian.geometricFloating);
                    asianAcc.buehlerArithFixed.add(lvAsian.arithmeticFixed);
                    asianAcc.buehlerArithFloating.add(lvAsian.arithmeticFloating);
                    asianAcc.bsGeomFloating.add(bsAsian.geometricFloating);
                    if (fixings.size() > 31)
                        asianAcc.bsArithFixedMc.add(bsAsian.arithmeticFixed);
                    asianAcc.bsArithFloating.add(bsAsian.arithmeticFloating);
                };

            const auto accumulateBarrier =
                [&](const std::vector<Date>& fixings, BarrierScheduleAccumulators& barrierAcc) {
                    OptionContractParams barrierP;
                    barrierP.expiry = acc.expiry;
                    barrierP.strike = acc.strikeF;
                    barrierP.isCall = true;
                    barrierP.barrierDown = 0.90 * acc.spotS;
                    barrierP.barrierUp = 1.10 * acc.spotS;
                    barrierP.observationDates = fixings;

                    const BarrierMcFourPayoffs lvBarrier =
                        BarrierMcBuehlerOption::priceAllPayoffsFromSavePath(lvBank, barrierP,
                                                                              buehler);
                    const BarrierMcFourPayoffs bsBarrier =
                        priceBsBarrierAllPayoffsFromSavePath(bsBank, barrierP, md);

                    barrierAcc.buehlerDownOut.add(lvBarrier.downOut);
                    barrierAcc.buehlerDownIn.add(lvBarrier.downIn);
                    barrierAcc.buehlerUpOut.add(lvBarrier.upOut);
                    barrierAcc.buehlerUpIn.add(lvBarrier.upIn);
                    barrierAcc.bsDownOut.add(bsBarrier.downOut);
                    barrierAcc.bsDownIn.add(bsBarrier.downIn);
                    barrierAcc.bsUpOut.add(bsBarrier.upOut);
                    barrierAcc.bsUpIn.add(bsBarrier.upIn);
                };

            const auto accumulateLookback =
                [&](const std::vector<Date>& fixings, LookbackScheduleAccumulators& lookbackAcc) {
                    OptionContractParams lookbackP;
                    lookbackP.expiry = acc.expiry;
                    lookbackP.strike = acc.strikeF;
                    lookbackP.isCall = true;
                    lookbackP.observationDates = fixings;

                    const LookbackMcTwoPayoffs lvLookback =
                        LookbackMcBuehlerOption::priceAllPayoffsFromSavePath(lvBank, lookbackP,
                                                                               buehler);
                    const LookbackMcTwoPayoffs bsLookback =
                        priceBsLookbackAllPayoffsFromSavePath(bsBank, lookbackP, md);

                    lookbackAcc.buehlerFixed.add(lvLookback.fixed);
                    lookbackAcc.buehlerFloating.add(lvLookback.floating);
                    lookbackAcc.bsFixed.add(bsLookback.fixed);
                    lookbackAcc.bsFloating.add(bsLookback.floating);
                };

            const auto accumulateAutocall =
                [&](const std::vector<Date>& fixings, const std::string& couponStyle,
                    AutocallScheduleAccumulators& autocallAcc) {
                    AutocallMcTerms terms;
                    terms.couponRatePerPeriod = kAutocallSanityCouponRatePerPeriod;
                    terms.barrierFractionOfSpot = 1.0;
                    terms.referenceNotional = acc.spotS;
                    terms.couponStyle = couponStyle;

                    OptionContractParams autocallP;
                    autocallP.expiry = acc.expiry;
                    autocallP.observationDates = fixings;

                    const BuehlerMcPathPricingResult lvAutocall =
                        AutocallMcBuehlerOption::priceFromSavePath(lvBank, autocallP, terms,
                                                                   buehler);
                    const BuehlerMcPathPricingResult bsAutocall =
                        priceBsAutocallFromSavePath(bsBank, autocallP, terms, md);

                    autocallAcc.buehlerAutocall.add(lvAutocall);
                    autocallAcc.bsAutocall.add(bsAutocall);
                };

            accumulateAsian(acc.dailyFixings, acc.dailyAsian);
            accumulateAsian(acc.monthlyFixings, acc.monthlyAsian);
            accumulateBarrier(acc.dailyFixings, acc.dailyBarrier);
            accumulateBarrier(acc.monthlyFixings, acc.monthlyBarrier);
            accumulateLookback(acc.dailyFixings, acc.dailyLookback);
            accumulateLookback(acc.monthlyFixings, acc.monthlyLookback);
            accumulateAutocall(acc.monthlyFixings, "phoenix", acc.monthlyAutocallPhoenix);
            accumulateAutocall(acc.monthlyFixings, "athena", acc.monthlyAutocallAthena);
            accumulateAutocall(acc.annualFixings, "phoenix", acc.annualAutocallPhoenix);
            accumulateAutocall(acc.annualFixings, "athena", acc.annualAutocallAthena);
        }
    }

    const auto printAsianSchedule =
        [&](const std::string& scheduleLabel, const std::vector<Date>& fixings,
            const AsianScheduleAccumulators& asianAcc, const Date& expiry, const Real strikeF,
            double& sumAbsErr) {
            std::cout << scheduleLabel << " fixings: " << fixings.size()
                      << " (mean over " << nSubbanks << " sub-banks x " << subbankSamples
                      << " paths, K=F in S)\n"
                      << std::flush;
            printAsianRow("asian_geom_fixed", asianAcc.buehlerGeomFixed.finish(),
                          priceBsGeometricAsianCallFlat(md, expiry, strikeF, fixings), sumAbsErr);
            printAsianRow("asian_geom_floating", asianAcc.buehlerGeomFloating.finish(),
                          asianAcc.bsGeomFloating.finish().value, sumAbsErr);
            const Real bsArithFixed =
                (fixings.size() <= 31)
                    ? priceBsArithmeticAsianCallFlat(md, expiry, strikeF, fixings)
                    : asianAcc.bsArithFixedMc.finish().value;
            printAsianRow("asian_arith_fixed", asianAcc.buehlerArithFixed.finish(), bsArithFixed,
                          sumAbsErr);
            printAsianRow("asian_arith_floating", asianAcc.buehlerArithFloating.finish(),
                          asianAcc.bsArithFloating.finish().value, sumAbsErr);
        };

    const auto printBarrierSchedule =
        [&](const std::string& scheduleLabel, const std::vector<Date>& fixings,
            const BarrierScheduleAccumulators& barrierAcc, double& sumAbsErr) {
            std::cout << scheduleLabel << " fixings: " << fixings.size()
                      << " (mean over " << nSubbanks << " sub-banks x " << subbankSamples
                      << " paths, barriers=90%/110% spot in S)\n"
                      << std::flush;
            printBarrierRow("barrier_down_out", barrierAcc.buehlerDownOut.finish(),
                            barrierAcc.bsDownOut.finish().value, sumAbsErr);
            printBarrierRow("barrier_down_in", barrierAcc.buehlerDownIn.finish(),
                            barrierAcc.bsDownIn.finish().value, sumAbsErr);
            printBarrierRow("barrier_up_out", barrierAcc.buehlerUpOut.finish(),
                            barrierAcc.bsUpOut.finish().value, sumAbsErr);
            printBarrierRow("barrier_up_in", barrierAcc.buehlerUpIn.finish(),
                            barrierAcc.bsUpIn.finish().value, sumAbsErr);
        };

    const auto printLookbackSchedule =
        [&](const std::string& scheduleLabel, const std::vector<Date>& fixings,
            const LookbackScheduleAccumulators& lookbackAcc, double& sumAbsErr) {
            std::cout << scheduleLabel << " fixings: " << fixings.size()
                      << " (mean over " << nSubbanks << " sub-banks x " << subbankSamples
                      << " paths, call legs, K=F)\n"
                      << std::flush;
            printLookbackRow("lookback_fixed_call", lookbackAcc.buehlerFixed.finish(),
                             lookbackAcc.bsFixed.finish().value, sumAbsErr);
            printLookbackRow("lookback_float_call", lookbackAcc.buehlerFloating.finish(),
                             lookbackAcc.bsFloating.finish().value, sumAbsErr);
        };

    const auto printAutocallSchedule =
        [&](const std::string& scheduleLabel, const std::vector<Date>& fixings,
            const AutocallScheduleAccumulators& phoenixAcc,
            const AutocallScheduleAccumulators& athenaAcc, double& sumAbsErr) {
            std::cout << scheduleLabel << " fixings: " << fixings.size()
                      << " (mean over " << nSubbanks << " sub-banks x " << subbankSamples
                      << " paths, coupon=" << kAutocallSanityCouponRatePerPeriod
                      << " x spot, barrier=100% spot)\n"
                      << std::flush;
            printAutocallRow("autocall_phoenix", phoenixAcc.buehlerAutocall.finish(),
                             phoenixAcc.bsAutocall.finish().value, sumAbsErr);
            printAutocallRow("autocall_athena", athenaAcc.buehlerAutocall.finish(),
                             athenaAcc.bsAutocall.finish().value, sumAbsErr);
        };

    std::cout << "\n-- Path MC: Asian (shared banks, sub-bank mean) --\n";
    printAsianTableHeader();

    for (Size t = 0; t < nTenors; ++t) {
        const TenorAccumulators& acc = tenorAcc[t];
        const int months = kAsianSanityTenorMonths[t];
        const auto bsProcess = makeQlRepoDividendBsProcess(md, acc.expiry, acc.strikeF);
        const std::string mat = maturityLabel(months);

        printTenorBanner(mat);

        printAsianSchedule("daily (all bank dates to expiry)", acc.dailyFixings, acc.dailyAsian,
                           acc.expiry, acc.strikeF, sumAbsErrAsian);
        printAsianSchedule("monthly (last bank date per month)", acc.monthlyFixings,
                           acc.monthlyAsian, acc.expiry, acc.strikeF, sumAbsErrAsian);

        std::cout << "\n-- Path MC: Barrier (shared banks, sub-bank mean) --\n";
        printAsianTableHeader();

        printBarrierSchedule("daily (all bank dates to expiry)", acc.dailyFixings,
                             acc.dailyBarrier, sumAbsErrBarrier);
        printBarrierSchedule("monthly (last bank date per month)", acc.monthlyFixings,
                             acc.monthlyBarrier, sumAbsErrBarrier);

        std::cout << "\n-- Path MC: Lookback (shared banks, sub-bank mean) --\n";
        printAsianTableHeader();

        printLookbackSchedule("daily (all bank dates to expiry)", acc.dailyFixings,
                              acc.dailyLookback, sumAbsErrLookback);
        printLookbackSchedule("monthly (last bank date per month)", acc.monthlyFixings,
                              acc.monthlyLookback, sumAbsErrLookback);

        std::cout << "\n-- Path MC: Autocall phoenix / athena (shared banks, sub-bank mean) --\n";
        printAsianTableHeader();

        printAutocallSchedule("monthly (last bank date per month)", acc.monthlyFixings,
                              acc.monthlyAutocallPhoenix, acc.monthlyAutocallAthena,
                              sumAbsErrAutocall);
        printAutocallSchedule("annual (last bank date per year)", acc.annualFixings,
                              acc.annualAutocallPhoenix, acc.annualAutocallAthena,
                              sumAbsErrAutocall);

        std::cout << "-- FD --\n";
        printFdTableHeader();

        OptionContractParams p;
        p.expiry = acc.expiry;
        p.strike = acc.strikeF;
        p.isCall = true;

        const std::vector<Date> monthlyExerciseDates = monthlyObservationDates(md, acc.expiry);

        printFdRow("european_call", priceBsEuropean(bsProcess, acc.expiry, acc.strikeF),
                   LvEuropeanFdBuehlerOption(p, BuehlerOptionPriceSpace::S, kFdTGridPerYear, kFdXGrid)
                       .price(buehler),
                   sumAbsErrFd);

        printFdRow("cash_digital_call", priceBsCashDigitalCall(md, acc.expiry, acc.strikeF),
                   LvDigitalFdBuehlerOption(p, BuehlerOptionPriceSpace::S, kFdTGridPerYear, kFdXGrid, false)
                       .price(buehler),
                   sumAbsErrFd);
        printFdRow("stock_digital_call", priceBsStockDigital(bsProcess, acc.expiry, acc.strikeF),
                   LvDigitalFdBuehlerOption(p, BuehlerOptionPriceSpace::S, kFdTGridPerYear, kFdXGrid, true)
                       .price(buehler),
                   sumAbsErrFd);

        const Real strikeLow = acc.strikeF * (1.0 - kRangeWidth);
        const Real strikeUp = acc.strikeF * (1.0 + kRangeWidth);
        OptionContractParams accrualP;
        accrualP.strikeLow = strikeLow;
        accrualP.strikeUp = strikeUp;
        accrualP.observationDates = monthlyExerciseDates;

        printFdRow("range_accrual",
                   priceBsRangeAccrual(md, monthlyExerciseDates, strikeLow, strikeUp),
                   LvDigitalAccrualFdBuehlerOption(accrualP, BuehlerOptionPriceSpace::S,
                                                   kFdTGridPerYear, kFdXGrid)
                       .price(buehler),
                   sumAbsErrFd);
    }

    const double meanAbsErrFd = sumAbsErrFd / static_cast<double>(nFdRows);
    const double meanAbsErrAsian = sumAbsErrAsian / static_cast<double>(nAsianRows);
    const double meanAbsErrBarrier = sumAbsErrBarrier / static_cast<double>(nBarrierRows);
    const double meanAbsErrLookback = sumAbsErrLookback / static_cast<double>(nLookbackRows);
    const double meanAbsErrAutocall = sumAbsErrAutocall / static_cast<double>(nAutocallRows);
    const Size nAllRows =
        nFdRows + nAsianRows + nBarrierRows + nLookbackRows + nAutocallRows;
    const double meanAbsErrAll =
        (sumAbsErrFd + sumAbsErrAsian + sumAbsErrBarrier + sumAbsErrLookback + sumAbsErrAutocall) /
        static_cast<double>(nAllRows);
    std::cout << "\nmean_abs_err (FD): " << meanAbsErrFd << " (" << nFdRows << " rows)\n";
    std::cout << "mean_abs_err (Asian): " << meanAbsErrAsian << " (" << nAsianRows << " rows)\n";
    std::cout << "mean_abs_err (Barrier): " << meanAbsErrBarrier << " (" << nBarrierRows
              << " rows)\n";
    std::cout << "mean_abs_err (Lookback): " << meanAbsErrLookback << " (" << nLookbackRows
              << " rows)\n";
    std::cout << "mean_abs_err (Autocall): " << meanAbsErrAutocall << " (" << nAutocallRows
              << " rows)\n";
    std::cout << "mean_abs_err (all): " << meanAbsErrAll << " (" << nAllRows << " rows)\n"
              << std::flush;
    return meanAbsErrAll;
}
