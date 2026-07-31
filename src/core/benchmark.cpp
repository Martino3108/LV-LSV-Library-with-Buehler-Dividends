/**
 * @file benchmark.cpp
 * @brief Timing benchmarks; see @c verify_fit, @c benchmark_pipeline_sanity, etc.
 */

#include "benchmark.h"
#include "asian_mc_buehler_option.h"
#include "benchmark_numeric.h"
#include "buehler_fixing_save_path.h"
#include "buehler_model.h"
#include "buehler_mc_settings.h"
#include "market_data.h"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <ql/quantlib.hpp>

void checkImportedData(const MarketData& md) {
    using namespace QuantLib;
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "=== Imported Data Check ===\n";
    std::cout << "Today: " << md.today() << "\n";
    std::cout << "Spot loaded: " << md.spotValue() << "\n\n";

    std::cout << "RFR nodal rates vs interpolated curve:\n";
    for (Size i = 0; i < md.riskFreeDates().size() && i < md.riskFreeZeroRates().size(); ++i) {
        const Date& d = md.riskFreeDates()[i];
        const Rate nodal = md.riskFreeZeroRates()[i];
        const Rate curveRate =
            md.riskFreeTs()->zeroRate(d, md.dayCounter(), Continuous, NoFrequency).rate();
        const Time t = md.dayCounter().yearFraction(md.today(), d);
        std::cout << "  t=" << t << " nodal=" << nodal << " curve=" << curveRate << "\n";
    }

    std::cout << "\nRepo nodal rates vs interpolated curve:\n";
    for (Size i = 0; i < md.repoDates().size() && i < md.repoZeroRates().size(); ++i) {
        const Date& d = md.repoDates()[i];
        const Rate nodal = md.repoZeroRates()[i];
        const Rate curveRate =
            md.repoTs()->zeroRate(d, md.dayCounter(), Continuous, NoFrequency).rate();
        const Time t = md.dayCounter().yearFraction(md.today(), d);
        std::cout << "  t=" << t << " nodal=" << nodal << " curve=" << curveRate << "\n";
    }

    std::cout << "\nDiscrete dividends loaded: " << md.dividendDates().size() << "\n";
    for (Size i = 0; i < md.dividendDates().size() && i < md.dividendAmounts().size(); ++i) {
        std::cout << "  " << md.dividendDates()[i] << " cash=" << md.dividendAmounts()[i];
        if (i < md.dividendProportional().size()) {
            std::cout << " proportional=" << md.dividendProportional()[i];
        }
        std::cout << "\n";
    }

    std::cout << "\nVol grid: nodal matrix vs BlackVolTermStructure:\n";
    for (Size i = 0; i < md.strikes().size(); ++i) {
        for (Size j = 0; j < md.expiries().size(); ++j) {
            const Real k = md.strikes()[i];
            const Date& expiry = md.expiries()[j];
            const Volatility nodal = md.impliedVols()[i][j];
            const Volatility surfaceVol = md.blackVolTs()->blackVol(expiry, k, true);
            const Time t = md.dayCounter().yearFraction(md.today(), expiry);
            std::cout << "  t=" << t << " K=" << k << " nodal=" << nodal
                      << " surface=" << surfaceVol << "\n";
        }
    }
    std::cout << "===========================\n";
}

/**
 * @copydoc benchmark_asian_fast_seed_sweep
 */
void benchmark_asian_fast_seed_sweep(MarketData& md,
                                     const QuantLib::Size fastWorkers,
                                     const QuantLib::Size mcSamples) {
    using namespace QuantLib;
    using clock = std::chrono::steady_clock;

    constexpr int kHorizonYears = 2;
    const Size nWorkers = std::max<Size>(Size(1), fastWorkers);
    constexpr int kOptionMaturityYears = 2;
    constexpr int kSeedCount = 50;
    constexpr BigNatural kSeedBase = 20001;
    std::vector<BigNatural> seeds;
    seeds.reserve(kSeedCount);
    for (int i = 0; i < kSeedCount; ++i)
        seeds.push_back(kSeedBase + static_cast<BigNatural>(i));
    /** Independent MC streams per engine (same row base seed, different offsets). */
    constexpr BigNatural kSeedOffsetQl = 0;
    constexpr BigNatural kSeedOffsetFast1 = 1'000'000'000ULL;
    constexpr BigNatural kSeedOffsetFastN = 2'000'000'000ULL;

    const Date horizonMax =
        dateFromBusiness252YearFraction(md.today(), static_cast<Real>(kHorizonYears), md.calendar());

    BuehlerModel model(md);
    model.preprocessing();
    model.calibration(/*runValidation=*/true);
    const Date expiry =
        dateFromBusiness252YearFraction(md.today(), static_cast<Real>(kOptionMaturityYears),
                                        md.calendar());
    const Real strikeF = model.forward0T(expiry);

    std::cout << std::fixed << std::setprecision(8);
    std::cout << "\n=== " << kHorizonYears << "Y Asian pricing: QL vs Fast@1 vs Fast@" << nWorkers << " ("
              << kSeedCount << " seeds, " << kSeedBase << "–" << (kSeedBase + kSeedCount - 1)
              << ") ===\n"
              << "market data from caller, horizon=" << kHorizonYears << "Y, mcSamples=" << mcSamples
              << " (antithetic), workers=" << nWorkers
              << " (single LV calibration + validation, MC-only per row)\n"
              << "MC seed per engine (row base S): ql=S+" << kSeedOffsetQl
              << ", fast1=S+" << kSeedOffsetFast1 << ", fastN=S+" << kSeedOffsetFastN << '\n';

    const auto monthlyFixingsToExpiry = [&](const Date& expiryDate, const BuehlerFixingSavePath& bank) {
        std::vector<Date> out;
        out.reserve(60);
        Date d = md.today();
        while (true) {
            d = md.calendar().advance(d, 1, Months, Following);
            if (d > expiryDate)
                break;
            if (bank.hasFixingDate(d))
                out.push_back(d);
        }
        if (out.empty() || out.back() != expiryDate)
            out.push_back(expiryDate);
        return out;
    };

    struct RunResult {
        double simMs = 0.0;
        double pricingMs = 0.0;
        double optionPrice = 0.0;
    };

    const auto runCase =
        [&](const bool useFast, const Size workers, const BigNatural seed) -> RunResult {
        BuehlerMcSettings mcSettings;
        mcSettings.mcSamples = mcSamples;
        mcSettings.seed = seed;
        mcSettings.dynamics = BuehlerMcDynamics::Lv;
        mcSettings.useFastPathSimulator = useFast;
        mcSettings.mcPathWorkers = useFast ? workers : 0;

        const clock::time_point t0Sim = clock::now();
        model.simulateFixingPaths(horizonMax, {}, mcSettings);
        const clock::time_point t1Sim = clock::now();
        const BuehlerFixingSavePath& bank = model.fixingSavePath();

        RunResult out;
        out.simMs = std::chrono::duration<double, std::milli>(t1Sim - t0Sim).count();

        const clock::time_point t0Pricing = clock::now();
        OptionContractParams p;
        p.expiry = expiry;
        p.strike = strikeF;
        p.isCall = true;
        p.observationDates = monthlyFixingsToExpiry(expiry, bank);
        const AsianMcFourPayoffs payoffs = AsianMcBuehlerOption::priceAllPayoffsFromSavePath(bank, p, model);
        out.optionPrice = benchmarkToDouble(payoffs.arithmeticFixed.value);
        const clock::time_point t1Pricing = clock::now();
        out.pricingMs = std::chrono::duration<double, std::milli>(t1Pricing - t0Pricing).count();
        return out;
    };

    std::cout << "seed   ql_total_ms   fast1_total_ms   fastN_total_ms   ql/fast1   ql/fastN"
                 "   parallel_gain   option_px_ql   option_px_f1   option_px_fN   px_diff(f1-ql)"
                 "   px_diff(fN-ql)\n";
    double sumQlFast1 = 0.0;
    double sumQlFastN = 0.0;
    double sumParallelGain = 0.0;
    double sumOptionQl = 0.0;
    double sumOptionF1 = 0.0;
    double sumOptionFN = 0.0;
    double sumPriceDiffF1Ql = 0.0;
    double sumPriceDiffVsQl = 0.0;
    for (const BigNatural baseSeed : seeds) {
        const BigNatural seedQl = baseSeed + kSeedOffsetQl;
        const BigNatural seedFast1 = baseSeed + kSeedOffsetFast1;
        const BigNatural seedFastN = baseSeed + kSeedOffsetFastN;
        const RunResult ql = runCase(false, 0, seedQl);
        const RunResult fast1 = runCase(true, 1, seedFast1);
        const RunResult fastN = runCase(true, nWorkers, seedFastN);
        const double tQl = ql.simMs + ql.pricingMs;
        const double t1 = fast1.simMs + fast1.pricingMs;
        const double tN = fastN.simMs + fastN.pricingMs;
        const double gainQlFast1 = tQl / std::max(t1, 1e-9);
        const double gainQlFastN = tQl / std::max(tN, 1e-9);
        const double parallelGain = t1 / std::max(tN, 1e-9);

        const double priceDiffF1Ql = fast1.optionPrice - ql.optionPrice;
        const double priceDiffVsQl = fastN.optionPrice - ql.optionPrice;
        sumQlFast1 += gainQlFast1;
        sumQlFastN += gainQlFastN;
        sumParallelGain += parallelGain;
        sumOptionQl += ql.optionPrice;
        sumOptionF1 += fast1.optionPrice;
        sumOptionFN += fastN.optionPrice;
        sumPriceDiffF1Ql += priceDiffF1Ql;
        sumPriceDiffVsQl += priceDiffVsQl;
        std::cout << baseSeed << "   " << tQl << "   " << t1 << "   " << tN << "   "
                  << gainQlFast1 << "x   " << gainQlFastN << "x   " << parallelGain
                  << "x   " << ql.optionPrice << "   " << fast1.optionPrice << "   "
                  << fastN.optionPrice << "   " << priceDiffF1Ql << "   " << priceDiffVsQl << '\n';
    }
    std::cout << "avg speedup ql/fast1: "
              << (sumQlFast1 / static_cast<double>(seeds.size())) << "x\n"
              << "avg speedup ql/fastN: "
              << (sumQlFastN / static_cast<double>(seeds.size())) << "x\n"
              << "avg parallel gain (fastN vs fast1): "
              << (sumParallelGain / static_cast<double>(seeds.size())) << "x\n"
              << "avg option px ql: "
              << (sumOptionQl / static_cast<double>(seeds.size())) << '\n'
              << "avg option px f1: "
              << (sumOptionF1 / static_cast<double>(seeds.size())) << '\n'
              << "avg option px fN: "
              << (sumOptionFN / static_cast<double>(seeds.size())) << '\n'
              << "avg option px diff (f1-ql): "
              << (sumPriceDiffF1Ql / static_cast<double>(seeds.size())) << '\n'
              << "avg option px diff (fN-ql): "
              << (sumPriceDiffVsQl / static_cast<double>(seeds.size())) << '\n';
}
