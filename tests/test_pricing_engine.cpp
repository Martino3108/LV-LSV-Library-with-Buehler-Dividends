/**
 * @file test_pricing_engine.cpp
 * @brief Unit / regression tests for the Buehler LV-LSV pricing engine.
 *
 * Self-contained runner (no external test framework). Each TEST() registers itself;
 * main() runs everything and reports failures. The calibrated sample model is built
 * once and shared across tests (calibration dominates the fixture cost).
 *
 * Run via CTest (`ctest --test-dir build`) or directly (`build/pricing_tests`).
 */

#include "buehler_fixing_path_simulate.h"
#include "buehler_fixing_save_path.h"
#include "buehler_mc_path_pricing.h"
#include "buehler_model.h"
#include "digital_mc_buehler_option.h"
#include "european_mc_buehler_option.h"
#include "lv_digital_fd_buehler_option.h"
#include "lv_european_fd_buehler_option.h"
#include "market_data.h"
#include "option.h"
#include <ql/pricingengines/blackformula.hpp>
#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace QuantLib;

// ---------------------------------------------------------------------------
// Minimal test framework
// ---------------------------------------------------------------------------

struct TestFailure {
    std::string message;
};

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

#define TEST(name)                                                    \
    static void test_##name();                                        \
    static const Registrar registrar_##name(#name, &test_##name);     \
    static void test_##name()

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::ostringstream oss__;                                                   \
            oss__ << __FILE__ << ':' << __LINE__ << ": CHECK failed: " << #cond;        \
            throw TestFailure{oss__.str()};                                             \
        }                                                                               \
    } while (false)

#define CHECK_CLOSE(a, b, tol)                                                          \
    do {                                                                                \
        const double a__ = static_cast<double>(a);                                      \
        const double b__ = static_cast<double>(b);                                      \
        if (!(std::fabs(a__ - b__) <= (tol))) {                                         \
            std::ostringstream oss__;                                                   \
            oss__ << __FILE__ << ':' << __LINE__ << ": CHECK_CLOSE failed: " << #a      \
                  << " = " << a__ << " vs " << #b << " = " << b__ << " (|diff| = "      \
                  << std::fabs(a__ - b__) << " > " << (tol) << ")";                     \
            throw TestFailure{oss__.str()};                                             \
        }                                                                               \
    } while (false)

// ---------------------------------------------------------------------------
// Shared fixture: sample snapshot, preprocessed + calibrated once
// ---------------------------------------------------------------------------

BuehlerModel& calibratedSampleModel() {
    static MarketData md = [] {
        MarketData m;
        m.loadSampleMarketSnapshot();
        return m;
    }();
    static BuehlerModel model = [] {
        BuehlerModel mdl(static_cast<const MarketData&>(md));
        mdl.preprocessing();
        mdl.calibration(/*runValidation=*/false);
        return mdl;
    }();
    return model;
}

const MarketData& sampleMarketData() {
    static MarketData md = [] {
        MarketData m;
        m.loadSampleMarketSnapshot();
        return m;
    }();
    return md;
}

Real meanTerminalX(const BuehlerFixingSavePath& bank) {
    const Size last = bank.numFixings() - 1;
    Real sum = 0.0;
    for (Size p = 0; p < bank.numPaths(); ++p)
        sum += bank.xLevel(p, last);
    return sum / static_cast<Real>(bank.numPaths());
}

/** ATM implied vol in X (F = K = df = 1) from a simulated bank at its last fixing. */
Real atmImpliedVolXFromBank(const BuehlerFixingSavePath& bank,
                            const BuehlerModel& model,
                            const Date& expiry) {
    OptionContractParams params;
    params.expiry = expiry;
    params.strike = 1.0; // ATM in X: forward of X is 1 by construction
    params.isCall = true;
    const BuehlerMcPathPricingResult priced = EuropeanMcBuehlerOption::priceFromSavePath(
        bank, params, model, BuehlerOptionPriceSpace::X);
    const Time tenor = model.dayCounter().yearFraction(model.today(), expiry);
    const Real stdDev =
        blackFormulaImpliedStdDev(QuantLib::Option::Call, 1.0, 1.0, priced.value, 1.0);
    return stdDev / std::sqrt(tenor);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

/** Float bank storage: xLevel round-trips at float precision, sLevel applies S = G·x + D. */
TEST(bank_float_storage_and_affine_map) {
    const std::vector<Date> fixings = {Date(15, January, 2026), Date(16, February, 2026)};
    const std::vector<BuehlerBankReal> x = {1.0f, 1.1f, 0.9f, 1.05f}; // path-major, 2 paths
    const std::vector<Real> carryD = {10.0, 12.0};
    const std::vector<Real> slopeG = {90.0, 88.0};

    const BuehlerFixingSavePath bank(fixings, 2, x, carryD, slopeG, /*mcBrownianSteps=*/5);

    CHECK(bank.numPaths() == 2);
    CHECK(bank.numFixings() == 2);
    CHECK(bank.hasFixingDate(fixings[1]));
    CHECK(!bank.hasFixingDate(Date(1, March, 2026)));

    CHECK_CLOSE(bank.xLevel(0, 0), 1.0, 1e-6);
    CHECK_CLOSE(bank.xLevel(1, 1), 1.05, 1e-6);
    // S = D(T) + G(T)·X, computed in double off the float bank.
    CHECK_CLOSE(bank.sLevel(0, 1), 12.0 + 88.0 * 1.1, 1e-4);
    CHECK_CLOSE(bank.sLevel(1, 0), 10.0 + 90.0 * 0.9, 1e-4);
}

/** Sub-bank accumulator: mean across banks and stderr from their dispersion. */
TEST(subbank_accumulator_stats) {
    McSubbankAccumulator acc;
    acc.add({10.0, 0.5});
    acc.add({12.0, 0.5});
    acc.add({11.0, 0.5});
    const BuehlerMcPathPricingResult out = acc.finish();
    CHECK_CLOSE(out.value, 11.0, 1e-12);
    // sample std = 1, stderr = 1/sqrt(3)
    CHECK_CLOSE(out.errorEstimate, 1.0 / std::sqrt(3.0), 1e-12);

    // Single bank: fall back to the within-bank estimate.
    McSubbankAccumulator single;
    single.add({10.0, 0.25});
    const BuehlerMcPathPricingResult one = single.finish();
    CHECK_CLOSE(one.value, 10.0, 1e-12);
    CHECK_CLOSE(one.errorEstimate, 0.25, 1e-12);
}

/** The martingale gate warns on stderr above threshold and stays silent below. */
TEST(martingale_drift_monitor_warns) {
    std::ostringstream captured;
    std::streambuf* old = std::cerr.rdbuf(captured.rdbuf());

    buehlerMcCheckMartingaleDrift({1.0, 1.0, 1.0}, 0.005, "test");
    const bool silentOnClean = captured.str().empty();

    buehlerMcCheckMartingaleDrift({1.1, 1.1, 1.1}, 0.005, "test");
    const bool warnsOnDrift = captured.str().find("martingale warning") != std::string::npos;

    captured.str("");
    buehlerMcCheckMartingaleDrift({1.1, 1.1, 1.1}, 0.0, "test"); // 0 disables
    const bool silentWhenDisabled = captured.str().empty();

    std::cerr.rdbuf(old);
    CHECK(silentOnClean);
    CHECK(warnsOnDrift);
    CHECK(silentWhenDisabled);
}

/** simulateFixingBank (const path) with restricted save dates stores only those fixings. */
TEST(save_date_restriction_shrinks_the_bank) {
    const BuehlerModel& model = calibratedSampleModel();
    const MarketData& md = sampleMarketData();
    const Date expiry = md.expiries()[3]; // ~1y pillar
    const Date mid = md.expiries()[1];    // ~0.25y pillar

    BuehlerMcSettings settings;
    settings.mcSamples = 4000;
    settings.dynamics = BuehlerMcDynamics::Lv;
    settings.mcSavePathFixingDates = {mid, expiry};
    settings.mcPathWorkers = 0;

    const BuehlerFixingSavePath bank = model.simulateFixingBank(expiry, {}, settings);
    CHECK(bank.numFixings() == 2);
    CHECK(bank.hasFixingDate(mid));
    CHECK(bank.hasFixingDate(expiry));
    CHECK(bank.numPaths() == 4000);
    // The Brownian evolution still ran on the full daily grid.
    CHECK(bank.mcBrownianSteps() > 200);
}

/** LV fast path: the unit-forward X stays a martingale at the horizon. */
TEST(lv_fast_path_is_martingale) {
    const BuehlerModel& model = calibratedSampleModel();
    const MarketData& md = sampleMarketData();
    const Date expiry = md.expiries()[3]; // ~1y

    BuehlerMcSettings settings;
    settings.mcSamples = 30000;
    settings.dynamics = BuehlerMcDynamics::Lv;
    settings.mcSavePathFixingDates = {expiry};
    settings.mcPathWorkers = 0;

    const BuehlerFixingSavePath bank = model.simulateFixingBank(expiry, {}, settings);
    // ~20% vol over 1y, antithetic 30k paths -> stderr of mean ~ 0.2/sqrt(30000) ~ 1.2e-3.
    CHECK_CLOSE(meanTerminalX(bank), 1.0, 5e-3);
}

/**
 * Pure-Bergomi nu^2 normalization (regression for the forward-variance offset bug):
 * with sigma_t = exp(nu·Y_t − nu²·Var(Y_t)) the spot variance is unit-normalized
 * (E[sigma_t²] = 1) for every nu, so the ATM implied vol in X can only *decrease*
 * as nu grows (concavity of Black in realized vol). Under the old c_T = Var(Y_t)
 * normalization the nu = 2 vol level was inflated by e^{(nu²−1)Var(Y_t)} and the
 * ATM vol came out far *above* the nu = 1 level.
 */
TEST(bergomi_nu2_normalization) {
    BuehlerModel& model = calibratedSampleModel();
    const MarketData& md = sampleMarketData();
    const Date expiry = md.expiries()[1]; // ~0.25y
    const BuehlerBergomiParams original = model.bergomiParams();

    BuehlerMcSettings settings;
    settings.mcSamples = 60000;
    settings.dynamics = BuehlerMcDynamics::Bergomi;
    settings.mcSavePathFixingDates = {expiry};
    settings.mcPathWorkers = 0;

    model.setBergomiParams({2.0, 1.0, -0.7});
    const BuehlerFixingSavePath bankNu1 = model.simulateFixingBank(expiry, {}, settings);
    const Real ivNu1 = atmImpliedVolXFromBank(bankNu1, model, expiry);

    model.setBergomiParams({2.0, 2.0, -0.7});
    const BuehlerFixingSavePath bankNu2 = model.simulateFixingBank(expiry, {}, settings);
    const Real ivNu2 = atmImpliedVolXFromBank(bankNu2, model, expiry);

    model.setBergomiParams(original);

    // Both must stay martingales.
    CHECK_CLOSE(meanTerminalX(bankNu1), 1.0, 2e-2);
    CHECK_CLOSE(meanTerminalX(bankNu2), 1.0, 4e-2);

    // Unit normalization anchors both ATM vols near 1.0 (mixing pushes them below).
    CHECK(ivNu1 > 0.8 && ivNu1 < 1.05);
    CHECK(ivNu2 > 0.5 && ivNu2 < 1.05);
    // The bug produced ivNu2 >> ivNu1 (~ +25% at this tenor); correct code keeps
    // ivNu2 at or below ivNu1 up to MC noise.
    CHECK(ivNu2 <= ivNu1 + 0.02);
}

/** LSV MC (bins + Bergomi driver) reprices the LV FD European at the money. */
TEST(lsv_mc_reprices_lv_fd_atm) {
    BuehlerModel& model = calibratedSampleModel();
    const MarketData& md = sampleMarketData();
    const Date expiry = md.expiries()[3]; // ~1y

    OptionContractParams params;
    params.expiry = expiry;
    params.strike = md.spotValue();
    params.isCall = true;

    const Real fdPrice =
        LvEuropeanFdBuehlerOption(params, BuehlerOptionPriceSpace::S, kDefaultFdTGridPerYear,
                                  kDefaultFdXGrid)
            .price(model);
    CHECK(fdPrice > 0.0);

    BuehlerMcSettings settings;
    settings.mcSamples = 50000;
    settings.dynamics = BuehlerMcDynamics::Lsv;
    settings.mcSavePathFixingDates = {expiry};
    settings.mcPathWorkers = 0;

    const BuehlerFixingSavePath bank = model.simulateFixingBank(expiry, {}, settings);
    const BuehlerMcPathPricingResult mc = EuropeanMcBuehlerOption::priceFromSavePath(
        bank, params, model, BuehlerOptionPriceSpace::S);

    // Budget: 5 within-bank stderr, floored at 50bp of premium for binning/Euler bias.
    const Real tolerance = std::max(5.0 * mc.errorEstimate, 0.005 * fdPrice);
    CHECK_CLOSE(mc.value, fdPrice, tolerance);
}

/** Streaming (Welford) payoff stats reproduce the two-pass helper to round-off. */
TEST(payoff_accumulator_matches_two_pass) {
    const std::vector<Real> payoffs = {0.0, 1.2, 0.7, 3.4, 0.0, 2.1, 0.05, 5.9};
    const Real scale = 0.97;

    BuehlerMcPayoffAccumulator acc;
    for (const Real payoff : payoffs)
        acc.add(payoff);

    const BuehlerMcPathPricingResult streamed = acc.finish(scale);
    const BuehlerMcPathPricingResult twoPass = buehlerMcStatsFromPayoffs(payoffs, scale);
    CHECK_CLOSE(streamed.value, twoPass.value, 1e-13);
    CHECK_CLOSE(streamed.errorEstimate, twoPass.errorEstimate, 1e-13);
}

/**
 * X quote space treats the strike as pure-X (regression for the strike-space
 * inconsistency): the MC cash digital in X at kx must equal the empirical
 * P(X_T > kx) on the same bank, and the FD digital with the same X quote must
 * interpret the strike identically (same convention as the European pricers).
 */
TEST(digital_x_strike_is_pure_x) {
    const BuehlerModel& model = calibratedSampleModel();
    const MarketData& md = sampleMarketData();
    const Date expiry = md.expiries()[3]; // ~1y

    BuehlerMcSettings settings;
    settings.mcSamples = 30000;
    settings.dynamics = BuehlerMcDynamics::Lv;
    settings.mcSavePathFixingDates = {expiry};
    settings.mcPathWorkers = 0;
    const BuehlerFixingSavePath bank = model.simulateFixingBank(expiry, {}, settings);

    OptionContractParams params;
    params.expiry = expiry;
    params.strike = 1.0; // pure-X strike (ATM-forward in X), NOT an S level
    params.isCall = true;

    const BuehlerMcPathPricingResult mc = DigitalMcBuehlerOption::priceFromSavePath(
        bank, params, model, BuehlerOptionPriceSpace::X, /*assetOrNothing=*/false);

    // Undiscounted X-space digital = empirical exercise probability on the bank.
    const Size fixIdx = bank.fixingIndex(expiry);
    Size hits = 0;
    for (Size p = 0; p < bank.numPaths(); ++p) {
        if (bank.xLevel(p, fixIdx) > params.strike)
            ++hits;
    }
    const Real empirical = static_cast<Real>(hits) / static_cast<Real>(bank.numPaths());
    CHECK_CLOSE(mc.value, empirical, 1e-12);

    // FD under the same LV dynamics prices the same exercise probability.
    const Real fd = LvDigitalFdBuehlerOption(params, BuehlerOptionPriceSpace::X,
                                             kDefaultFdTGridPerYear, kDefaultFdXGrid,
                                             /*assetOrNothing=*/false)
                        .price(model);
    CHECK(fd > 0.0 && fd < 1.0);
    CHECK_CLOSE(mc.value, fd, std::max(5.0 * mc.errorEstimate, 0.01));
}

/** Calibration health: validation passes and the Dupire Black fallback share is small. */
TEST(calibration_validation_and_fallbacks) {
    const BuehlerModel& model = calibratedSampleModel();

    BuehlerCalibrationValidationOptions options;
    options.throwOnFailure = false;
    options.verbose = false;
    const BuehlerCalibrationValidationReport report = model.validate_calibration(options);
    CHECK(report.staticArbitrageOk);
    CHECK(report.smileFitOk);
    CHECK(report.passed());

    const BuehlerLvDenseRepairCounts& repair = model.lastLvDenseRepairCounts();
    CHECK(repair.denseGridCells > 0);
    const double fallbackFraction = static_cast<double>(repair.dupireBlackFallbacks) /
                                    static_cast<double>(repair.denseGridCells);
    CHECK(fallbackFraction <= kDupireBlackFallbackWarnFraction);
}

} // namespace

int main() {
    int failed = 0;
    for (const TestCase& test : registry()) {
        std::cout << "[ RUN      ] " << test.name << std::endl;
        try {
            test.fn();
            std::cout << "[       OK ] " << test.name << std::endl;
        } catch (const TestFailure& failure) {
            ++failed;
            std::cout << "[  FAILED  ] " << test.name << "\n    " << failure.message
                      << std::endl;
        } catch (const std::exception& ex) {
            ++failed;
            std::cout << "[  FAILED  ] " << test.name << "\n    unexpected exception: "
                      << ex.what() << std::endl;
        }
    }
    std::cout << registry().size() - failed << " / " << registry().size() << " tests passed"
              << std::endl;
    return failed == 0 ? 0 : 1;
}
