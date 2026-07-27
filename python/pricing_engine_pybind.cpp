/**
 * @file pricing_engine_pybind.cpp
 * @brief pybind11 module `pricing_engine`: @c PricingContext, JSON book pricing, verify grids.
 *
 * User-facing API and notebooks live under @c python/notebooks/; JSON schema under @c data/pricing_json_schema.yaml.
 */

#include "asian_mc_buehler_option.h"
#include "autocall_mc_buehler_option.h"
#include "barrier_mc_buehler_option.h"
#include "buehler_iv_x_arbitrage.h"
#include "buehler_mc_path_pricing.h"
#include "buehler_model.h"
#include "buehler_mc_settings.h"
#include "digital_accrual_mc_buehler_option.h"
#include "digital_mc_buehler_option.h"
#include "european_mc_buehler_option.h"
#include "lookback_mc_buehler_option.h"
#include "lv_digital_fd_buehler_option.h"
#include "lv_european_fd_buehler_option.h"
#include "market_data.h"
#include "verify_fit.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <memory>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <sstream>
#include <stdexcept>

#if defined(_OPENMP)
#include <omp.h>
#endif

namespace py = pybind11;
using namespace QuantLib;

namespace {

Date parseIsoDate(const std::string& iso) {
    std::istringstream ss(iso);
    int y = 0;
    int m = 0;
    int d = 0;
    char sep1 = 0;
    char sep2 = 0;
    ss >> y >> sep1 >> m >> sep2 >> d;
    if (!ss || sep1 != '-' || sep2 != '-' || m < 1 || m > 12 || d < 1 || d > 31) {
        throw std::invalid_argument("expected ISO date YYYY-MM-DD, got: " + iso);
    }
    return Date(d, static_cast<Month>(m), y);
}

std::string formatIsoDate(const Date& d) {
    std::ostringstream oss;
    oss << d.year() << '-' << static_cast<int>(d.month()) << '-' << d.dayOfMonth();
    return oss.str();
}

/** Notebook API: split MC into fixed-size banks when mc_samples exceeds cap. */
void notebookMcSubbankLayout(const Size mcSamples, Size& nSubbanks, Size& subbankSamples) {
    constexpr Size kCap = 50000;
    if (mcSamples == 0) {
        throw std::invalid_argument("mc_samples must be positive");
    }
    if (mcSamples <= kCap) {
        nSubbanks = 1;
        subbankSamples = mcSamples;
        return;
    }
    subbankSamples = kCap;
    nSubbanks = (mcSamples + kCap - 1) / kCap;
}

Date expiryNearYears(const MarketData& md, const double years) {
    const auto& expiries = md.expiries();
    if (expiries.empty()) {
        throw std::runtime_error("expiryNearYears: no expiries on market data");
    }
    Size best = 0;
    Real bestDist = std::numeric_limits<Real>::max();
    for (Size i = 0; i < expiries.size(); ++i) {
        const Real t = md.dayCounter().yearFraction(md.today(), expiries[i]);
        const Real dist = std::fabs(t - years);
        if (dist < bestDist) {
            bestDist = dist;
            best = i;
        }
    }
    return expiries[best];
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool dictHas(const py::dict& d, const char* key) {
    return d.contains(py::str(key)) && !d[py::str(key)].is_none();
}

std::string dictRequiredString(const py::dict& d, const char* key) {
    if (!dictHas(d, key)) {
        throw std::invalid_argument(std::string("missing required field: ") + key);
    }
    return py::cast<std::string>(d[py::str(key)]);
}

std::string dictOptionalString(const py::dict& d, const char* key,
                               const std::string& default_value = {}) {
    if (!dictHas(d, key)) {
        return default_value;
    }
    return py::cast<std::string>(d[py::str(key)]);
}

double dictOptionalDouble(const py::dict& d, const char* key, const double default_value) {
    if (!dictHas(d, key)) {
        return default_value;
    }
    return py::cast<double>(d[py::str(key)]);
}

bool dictOptionalBool(const py::dict& d, const char* key, const bool default_value) {
    if (!dictHas(d, key)) {
        return default_value;
    }
    return py::cast<bool>(d[py::str(key)]);
}

std::vector<std::string> dictOptionalStringList(const py::dict& d, const char* key) {
    std::vector<std::string> out;
    if (!dictHas(d, key)) {
        return out;
    }
    for (const py::handle item : d[py::str(key)]) {
        out.push_back(py::cast<std::string>(item));
    }
    return out;
}

std::optional<double> dictOptionalReal(const py::dict& d, const char* key) {
    if (!dictHas(d, key)) {
        return std::nullopt;
    }
    return py::cast<double>(d[py::str(key)]);
}

BuehlerOptionPriceSpace parseQuoteSpace(const std::string& quote_space) {
    return toLower(quote_space) == "s" ? BuehlerOptionPriceSpace::S : BuehlerOptionPriceSpace::X;
}

BarrierMcType parseBarrierType(const std::string& barrier_type) {
    const std::string value = toLower(barrier_type);
    if (value == "down_out") {
        return BarrierMcType::DownOut;
    }
    if (value == "down_in") {
        return BarrierMcType::DownIn;
    }
    if (value == "up_out") {
        return BarrierMcType::UpOut;
    }
    if (value == "up_in") {
        return BarrierMcType::UpIn;
    }
    throw std::invalid_argument("barrier_type must be down_out, down_in, up_out, or up_in");
}

AsianMcPayoffKind parseAsianPayoffKind(const std::string& average_type,
                                       const std::string& strike_style) {
    const std::string avg = toLower(average_type.empty() ? "arithmetic" : average_type);
    const std::string style = toLower(strike_style.empty() ? "fixed" : strike_style);
    if (avg == "geometric" && style == "floating") {
        return AsianMcPayoffKind::GeometricFloating;
    }
    if (avg == "geometric") {
        return AsianMcPayoffKind::GeometricFixed;
    }
    if (style == "floating") {
        return AsianMcPayoffKind::ArithmeticFloating;
    }
    return AsianMcPayoffKind::ArithmeticFixed;
}

LookbackMcStrikeStyle parseLookbackStrikeStyle(const std::string& strike_style) {
    return toLower(strike_style) == "floating" ? LookbackMcStrikeStyle::Floating
                                             : LookbackMcStrikeStyle::Fixed;
}

Real levelFromFractionOrAbsolute(const py::dict& spec, const char* fraction_key,
                                 const char* absolute_key, const Real spot) {
    if (dictHas(spec, fraction_key)) {
        return dictOptionalDouble(spec, fraction_key, 0.0) * spot;
    }
    if (dictHas(spec, absolute_key)) {
        return dictOptionalDouble(spec, absolute_key, Null<Real>());
    }
    return Null<Real>();
}

/** Resolve `expiry` (ISO) or `expiry_years` (year fraction from asof, adjusted to the
 *  first TARGET business day with Following convention). Mutually exclusive; one required. */
Date expiryFromSpec(const py::dict& spec, const Date& today, const Calendar& calendar) {
    const bool hasIso = dictHas(spec, "expiry");
    const bool hasYears = dictHas(spec, "expiry_years");
    if (hasIso && hasYears) {
        throw std::invalid_argument("expiry and expiry_years are mutually exclusive");
    }
    if (hasIso) {
        return parseIsoDate(dictRequiredString(spec, "expiry"));
    }
    if (hasYears) {
        const double years = py::cast<double>(spec[py::str("expiry_years")]);
        if (years <= 0.0) {
            throw std::invalid_argument("expiry_years must be positive");
        }
        const Date nominal = today + static_cast<Integer>(std::lround(years * 365.0));
        return calendar.adjust(nominal, Following);
    }
    throw std::invalid_argument("missing required field: expiry (or expiry_years)");
}

OptionContractParams optionParamsFromDict(const py::dict& spec, const Real spot,
                                          const Date& today, const Calendar& calendar) {
    OptionContractParams params;
    params.expiry = expiryFromSpec(spec, today, calendar);
    params.isCall = dictOptionalBool(spec, "is_call", true);

    if (dictHas(spec, "strike_fraction_of_spot") || dictHas(spec, "strike")) {
        params.strike = levelFromFractionOrAbsolute(spec, "strike_fraction_of_spot", "strike", spot);
    }
    if (dictHas(spec, "barrier_up_fraction_of_spot") || dictHas(spec, "barrier_up")) {
        params.barrierUp =
            levelFromFractionOrAbsolute(spec, "barrier_up_fraction_of_spot", "barrier_up", spot);
    }
    if (dictHas(spec, "barrier_down_fraction_of_spot") || dictHas(spec, "barrier_down")) {
        params.barrierDown = levelFromFractionOrAbsolute(spec, "barrier_down_fraction_of_spot",
                                                         "barrier_down", spot);
    }
    if (dictHas(spec, "strike_low_fraction_of_spot") || dictHas(spec, "strike_low")) {
        params.strikeLow =
            levelFromFractionOrAbsolute(spec, "strike_low_fraction_of_spot", "strike_low", spot);
    }
    if (dictHas(spec, "strike_up_fraction_of_spot") || dictHas(spec, "strike_up")) {
        params.strikeUp =
            levelFromFractionOrAbsolute(spec, "strike_up_fraction_of_spot", "strike_up", spot);
    }

    for (const std::string& iso : dictOptionalStringList(spec, "observation_dates")) {
        params.observationDates.push_back(parseIsoDate(iso));
    }

    const std::string frequency =
        toLower(dictOptionalString(spec, "observation_frequency", "monthly"));
    if (frequency == "daily") {
        params.observationFrequency = McObservationFrequency::Daily;
    } else if (frequency == "monthly") {
        params.observationFrequency = McObservationFrequency::Monthly;
    } else {
        throw std::invalid_argument("observation_frequency must be daily or monthly");
    }
    if (!params.observationDates.empty() && dictHas(spec, "observation_frequency")) {
        throw std::invalid_argument(
            "observation_frequency and observation_dates are mutually exclusive");
    }
    return params;
}

struct PricedOptionResult {
    std::string id;
    double value = std::numeric_limits<double>::quiet_NaN();
    double stderr = std::numeric_limits<double>::quiet_NaN();
    std::string status;
};

/** Parsed option spec (pure C++; safe to price in parallel). */
struct ParsedOptionSpec {
    std::string id;
    std::string product;
    OptionContractParams params;
    BuehlerOptionPriceSpace quote_space = BuehlerOptionPriceSpace::S;
    bool asset_or_nothing = false;
    AsianMcPayoffKind asian_kind = AsianMcPayoffKind::ArithmeticFixed;
    BarrierMcType barrier_type = BarrierMcType::DownOut;
    LookbackMcStrikeStyle lookback_style = LookbackMcStrikeStyle::Fixed;
    AutocallMcTerms autocall_terms;
};

ParsedOptionSpec parseOptionSpec(const py::dict& spec, const Real spot, const Date& today,
                                 const Calendar& calendar) {
    ParsedOptionSpec out;
    out.id = dictRequiredString(spec, "id");
    out.product = toLower(dictRequiredString(spec, "product"));
    out.quote_space = parseQuoteSpace(dictOptionalString(spec, "quote_space", "S"));
    if (out.quote_space == BuehlerOptionPriceSpace::X) {
        // Only these pricers honor the X quote space; the others (asian, barrier,
        // lookback, autocall) are S-space payoffs and would silently misprice.
        const bool supportsX = (out.product == "european" || out.product == "digital" ||
                                out.product == "digital_accrual");
        if (!supportsX)
            throw std::invalid_argument("quote_space 'X' is only supported for european, "
                                        "digital and digital_accrual (got '" +
                                        out.product + "')");
    }
    // X-quoted contracts are written on the spot-normalized martingale X, so
    // "fraction of spot" and pure-X level coincide: parse them against a unit spot
    // (strikes land directly in X space, matching the pricers' convention).
    const Real levelScale = (out.quote_space == BuehlerOptionPriceSpace::X) ? 1.0 : spot;
    out.params = optionParamsFromDict(spec, levelScale, today, calendar);
    out.asset_or_nothing = dictOptionalBool(spec, "asset_or_nothing", false);
    out.asian_kind =
        parseAsianPayoffKind(dictOptionalString(spec, "average_type", "arithmetic"),
                             dictOptionalString(spec, "strike_style", "fixed"));
    if (out.product == "barrier") {
        out.barrier_type = parseBarrierType(dictRequiredString(spec, "barrier_type"));
    }
    out.lookback_style =
        parseLookbackStrikeStyle(dictOptionalString(spec, "strike_style", "fixed"));

    out.autocall_terms.couponRatePerPeriod =
        dictOptionalDouble(spec, "coupon_rate_per_period", 0.03);
    out.autocall_terms.barrierFractionOfSpot =
        dictOptionalDouble(spec, "barrier_fraction_of_spot", 1.0);
    out.autocall_terms.couponStyle = dictOptionalString(spec, "coupon_style", "phoenix");
    if (dictHas(spec, "reference_notional")) {
        out.autocall_terms.referenceNotional = dictOptionalDouble(spec, "reference_notional", spot);
    } else if (dictHas(spec, "reference_notional_fraction_of_spot")) {
        out.autocall_terms.referenceNotional =
            dictOptionalDouble(spec, "reference_notional_fraction_of_spot", 1.0) * spot;
    } else {
        out.autocall_terms.referenceNotional = spot;
    }
    return out;
}

BuehlerMcPathPricingResult priceSpecOnSavePath(const ParsedOptionSpec& spec,
                                               const BuehlerFixingSavePath& bank,
                                               const BuehlerModel& model) {
    const std::string& product = spec.product;
    const OptionContractParams& params = spec.params;

    if (product == "european") {
        return EuropeanMcBuehlerOption::priceFromSavePath(bank, params, model, spec.quote_space);
    }
    if (product == "digital") {
        return DigitalMcBuehlerOption::priceFromSavePath(bank, params, model, spec.quote_space,
                                                         spec.asset_or_nothing);
    }
    if (product == "digital_accrual") {
        return DigitalAccrualMcBuehlerOption::priceFromSavePath(bank, params, model,
                                                                spec.quote_space);
    }
    if (product == "asian") {
        return AsianMcBuehlerOption::priceFromSavePath(bank, spec.asian_kind, params, model);
    }
    if (product == "barrier") {
        return BarrierMcBuehlerOption::priceFromSavePath(bank, params, model, spec.barrier_type);
    }
    if (product == "lookback") {
        return LookbackMcBuehlerOption::priceFromSavePath(bank, params, model,
                                                          spec.lookback_style);
    }
    if (product == "autocall") {
        return AutocallMcBuehlerOption::priceFromSavePath(bank, params, spec.autocall_terms,
                                                          model);
    }

    throw std::invalid_argument("unsupported product: " + product);
}

/** Owns market data + model so Python does not need to manage object lifetimes. */
class PricingContext {
public:
    static std::unique_ptr<PricingContext> from_tables(const std::string& asof, const double spot,
                                                      const std::vector<double>& rfr_tenor_years,
                                                      const std::vector<double>& rfr_zero_rates,
                                                      const std::vector<double>& repo_tenor_years,
                                                      const std::vector<double>& repo_zero_rates,
                                                      const std::vector<double>& vol_tenor_years,
                                                      const std::vector<double>& vol_strikes,
                                                      const std::vector<double>& implied_vols,
                                                      const std::vector<std::string>& dividend_dates,
                                                      const std::vector<double>& dividend_amounts,
                                                      const double bergomi_k,
                                                      const double bergomi_nu,
                                                      const double bergomi_rho) {
        MarketDataTables tables;
        tables.asof = asof;
        tables.spot = static_cast<Real>(spot);
        tables.rfrTenorYears.assign(rfr_tenor_years.begin(), rfr_tenor_years.end());
        tables.rfrZeroRates.assign(rfr_zero_rates.begin(), rfr_zero_rates.end());
        tables.repoTenorYears.assign(repo_tenor_years.begin(), repo_tenor_years.end());
        tables.repoZeroRates.assign(repo_zero_rates.begin(), repo_zero_rates.end());
        tables.volTenorYears.assign(vol_tenor_years.begin(), vol_tenor_years.end());
        tables.volStrikes.assign(vol_strikes.begin(), vol_strikes.end());
        tables.impliedVols.assign(implied_vols.begin(), implied_vols.end());
        tables.dividendDates = dividend_dates;
        tables.dividendAmounts.assign(dividend_amounts.begin(), dividend_amounts.end());
        tables.bergomiK = static_cast<Real>(bergomi_k);
        tables.bergomiNu = static_cast<Real>(bergomi_nu);
        tables.bergomiRho = static_cast<Real>(bergomi_rho);

        auto ctx = std::make_unique<PricingContext>();
        ctx->market_data_ = std::make_unique<MarketData>();
        ctx->market_data_->loadFromTables(tables);
        ctx->rebuild_model();
        return ctx;
    }

    static std::unique_ptr<PricingContext> from_sample() {
        auto ctx = std::make_unique<PricingContext>();
        ctx->market_data_ = std::make_unique<MarketData>();
        ctx->market_data_->loadSampleMarketSnapshot();
        ctx->rebuild_model();
        return ctx;
    }

    static std::unique_ptr<PricingContext> from_mock() {
        auto ctx = std::make_unique<PricingContext>();
        ctx->market_data_ = std::make_unique<MarketData>();
        ctx->market_data_->loadConstantMock();
        ctx->rebuild_model();
        return ctx;
    }

    void preprocessing() { model_->preprocessing(); }

    void calibration(const bool run_validation) { model_->calibration(run_validation); }


    BuehlerCalibrationValidationReport validate_calibration(
        BuehlerCalibrationValidationOptions options) const {
        return model_->validate_calibration(options);
    }

    BuehlerImpliedVolXArbitrageReport check_static_arbitrage(const Size n_time_samples,
                                                             const Size n_strike_samples,
                                                             const double tol_butterfly,
                                                             const double tol_calendar) const {
        return ::check_static_arbitrage(*model_, n_time_samples, n_strike_samples, tol_butterfly,
                                        tol_calendar, false);
    }

    struct SimulateResult {
        double sim_ms = 0.0;
        std::string expiry;
        Size mc_samples = 0;
        std::string dynamics;
    };

    /** Shared assembly of MC settings for simulate_paths / price_book.
     *  @param lsv_bins 0 keeps the engine default (@c kDefaultLsvBins).
     *  @param path_workers 0 keeps the environment default (@c BUEHLER_MC_PATH_WORKERS). */
    static BuehlerMcSettings makeMcSettings(const Size mc_samples, const BigNatural seed,
                                            const std::string& dynamics,
                                            const std::vector<std::string>& save_fixing_dates,
                                            const Size lsv_bins, const Size path_workers) {
        BuehlerMcSettings settings;
        settings.mcSamples = mc_samples;
        settings.seed = seed;
        settings.lsvBins = (lsv_bins > 0) ? lsv_bins : kDefaultLsvBins;
        settings.mcPathWorkers =
            (path_workers > 0) ? path_workers : buehlerMcPathWorkersFromEnvironment();
        for (const std::string& iso : save_fixing_dates) {
            settings.mcSavePathFixingDates.push_back(parseIsoDate(iso));
        }

        if (dynamics == "lv") {
            settings.dynamics = BuehlerMcDynamics::Lv;
        } else if (dynamics == "lsv") {
            settings.dynamics = BuehlerMcDynamics::Lsv;
        } else {
            throw std::invalid_argument("dynamics must be 'lv' or 'lsv'");
        }
        return settings;
    }

    SimulateResult simulate_paths(const std::string& expiry_iso, const Size mc_samples,
                                  const BigNatural seed, const std::string& dynamics,
                                  const std::vector<std::string>& save_fixing_dates,
                                  const Size lsv_bins, const Size path_workers) {
        const Date expiry = parseIsoDate(expiry_iso);
        const BuehlerMcSettings settings =
            makeMcSettings(mc_samples, seed, dynamics, save_fixing_dates, lsv_bins, path_workers);

        const auto t0 = std::chrono::steady_clock::now();
        model_->simulateFixingPaths(expiry, {}, settings);
        const auto t1 = std::chrono::steady_clock::now();

        SimulateResult out;
        out.sim_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        out.expiry = expiry_iso;
        out.mc_samples = mc_samples;
        out.dynamics = dynamics;
        return out;
    }

    SimulateResult simulate_paths_near_years(const double horizon_years, const Size mc_samples,
                                             const BigNatural seed, const std::string& dynamics,
                                             const std::vector<std::string>& save_fixing_dates,
                                             const Size lsv_bins, const Size path_workers) {
        const Date expiry = expiryNearYears(*market_data_, horizon_years);
        return simulate_paths(formatIsoDate(expiry), mc_samples, seed, dynamics,
                              save_fixing_dates, lsv_bins, path_workers);
    }

    struct MarketSummary {
        std::string asof;
        double spot = 0.0;
        std::vector<std::string> expiries;
        std::vector<double> expiry_years;
        std::vector<double> strikes;
        std::vector<double> implied_vols;
        std::vector<std::string> dividend_dates;
        std::vector<double> dividend_amounts;
        std::vector<double> rfr_tenor_years;
        std::vector<double> rfr_zero_rates;
        std::vector<double> repo_tenor_years;
        std::vector<double> repo_zero_rates;
        double bergomi_k = 0.0;
        double bergomi_nu = 0.0;
        double bergomi_rho = 0.0;
    };

    MarketSummary market_summary() const {
        MarketSummary summary;
        summary.asof = formatIsoDate(market_data_->today());
        summary.spot = static_cast<double>(market_data_->spotValue());
        summary.expiries.reserve(market_data_->expiries().size());
        summary.expiry_years.reserve(market_data_->expiries().size());
        for (const Date& expiry : market_data_->expiries()) {
            summary.expiries.push_back(formatIsoDate(expiry));
            summary.expiry_years.push_back(static_cast<double>(
                market_data_->dayCounter().yearFraction(market_data_->today(), expiry)));
        }
        summary.strikes.assign(market_data_->strikes().begin(), market_data_->strikes().end());
        const Matrix& vols = market_data_->impliedVols();
        summary.implied_vols.reserve(vols.rows() * vols.columns());
        for (Size i = 0; i < vols.rows(); ++i) {
            for (Size j = 0; j < vols.columns(); ++j) {
                summary.implied_vols.push_back(static_cast<double>(vols[i][j]));
            }
        }
        for (const Date& d : market_data_->dividendDates()) {
            summary.dividend_dates.push_back(formatIsoDate(d));
        }
        summary.dividend_amounts.assign(market_data_->dividendAmounts().begin(),
                                        market_data_->dividendAmounts().end());
        for (const Date& d : market_data_->riskFreeDates()) {
            summary.rfr_tenor_years.push_back(static_cast<double>(
                market_data_->dayCounter().yearFraction(market_data_->today(), d)));
        }
        summary.rfr_zero_rates.assign(market_data_->riskFreeZeroRates().begin(),
                                      market_data_->riskFreeZeroRates().end());
        for (const Date& d : market_data_->repoDates()) {
            summary.repo_tenor_years.push_back(static_cast<double>(
                market_data_->dayCounter().yearFraction(market_data_->today(), d)));
        }
        summary.repo_zero_rates.assign(market_data_->repoZeroRates().begin(),
                                         market_data_->repoZeroRates().end());
        summary.bergomi_k = static_cast<double>(market_data_->bergomiK());
        summary.bergomi_nu = static_cast<double>(market_data_->bergomiNu());
        summary.bergomi_rho = static_cast<double>(market_data_->bergomiRho());
        return summary;
    }

    /** X quote space: the fraction is used directly as the pure-X strike (X is spot-normalized).
     *  Grid arguments default to the production constants; non-default values are for
     *  convergence studies (label-error attribution), not for label generation. */
    double price_european_fd(const std::string& expiry_iso, const double strike_fraction_of_spot,
                             const bool is_call, const std::string& quote_space,
                             const Size t_grid_per_year, const Size x_grid) const {
        const BuehlerOptionPriceSpace space = parseQuoteSpace(quote_space);
        const Real levelScale =
            (space == BuehlerOptionPriceSpace::X) ? 1.0 : market_data_->spotValue();
        OptionContractParams params;
        params.expiry = parseIsoDate(expiry_iso);
        params.strike = static_cast<Real>(strike_fraction_of_spot) * levelScale;
        params.isCall = is_call;
        return static_cast<double>(
            LvEuropeanFdBuehlerOption(params, space, t_grid_per_year, x_grid).price(*model_));
    }

    /** X quote space: the fraction is used directly as the pure-X strike (X is spot-normalized).
     *  Grid arguments default to the production constants; non-default values are for
     *  convergence studies (label-error attribution), not for label generation. */
    double price_digital_fd(const std::string& expiry_iso, const double strike_fraction_of_spot,
                            const bool asset_or_nothing, const std::string& quote_space,
                            const Size t_grid_per_year, const Size x_grid) const {
        const BuehlerOptionPriceSpace space = parseQuoteSpace(quote_space);
        const Real levelScale =
            (space == BuehlerOptionPriceSpace::X) ? 1.0 : market_data_->spotValue();
        OptionContractParams params;
        params.expiry = parseIsoDate(expiry_iso);
        params.strike = static_cast<Real>(strike_fraction_of_spot) * levelScale;
        params.isCall = true;
        return static_cast<double>(
            LvDigitalFdBuehlerOption(params, space, t_grid_per_year, x_grid, asset_or_nothing)
                .price(*model_));
    }

    /** Same grid as C++ @c verify_LV_BS_consistency (S-space IV); returns rows for Python plots. */
    std::vector<LvIvFitRow> verify_lv_bs_consistency(const Size t_grid_per_year,
                                                     const Size x_grid) const {
        return collect_lv_iv_fit_grid(*market_data_, *model_, t_grid_per_year, x_grid);
    }

    /** Dense fixed pure-X LV grid (same 500×1000 export as C++ @c verify_LV_BS_consistency). */
    std::string export_lv_fixed_x_csv(const std::string& output_path) const {
        ::export_lv_fixed_x_csv(*model_, output_path, false);
        return output_path;
    }

    /** Fixed LV σ(t, kx) on market vol-surface pillars (flat kx extrapolation outside tab support). */
    LvFixedXMarketGrid fixed_lv_x_market_grid() const {
        return collect_lv_fixed_x_market_grid(*market_data_, *model_);
    }

    /** Dupire tabulated σ_LV on @c denseXStrikes × @c denseExpiries (engine storage grid). */
    LvFixedXTabulatedGrid fixed_lv_x_tabulated_grid() const {
        return collect_lv_fixed_x_tabulated_grid(*model_);
    }

    /** Same grid as C++ @c verify_lsv_mc_vs_lv_fd; notebook sets banks from @p mc_samples only. */
    std::vector<LsvVsLvRow> verify_lsv_mc_vs_lv_fd(const Size mc_samples, const Size t_grid_per_year,
                                                   const Size x_grid) {
        Size nSubbanks = 0;
        Size subbankSamples = 0;
        notebookMcSubbankLayout(mc_samples, nSubbanks, subbankSamples);
        return collect_lsv_vs_lv_grid(*market_data_, *model_, nSubbanks, subbankSamples,
                                      t_grid_per_year, x_grid);
    }

    std::vector<PricedOptionResult> price_all(const py::list& specs) const {
        if (!model_->hasFixingSavePath()) {
            throw std::runtime_error("call simulate_paths() before price_all()");
        }

        const BuehlerFixingSavePath& bank = model_->fixingSavePath();
        const Real spot = market_data_->spotValue();
        const BuehlerModel& model = *model_;

        std::vector<ParsedOptionSpec> parsed;
        parsed.reserve(specs.size());
        for (const py::handle item : specs) {
            parsed.push_back(parseOptionSpec(py::cast<py::dict>(item), spot,
                                             market_data_->today(), market_data_->calendar()));
        }

        std::vector<PricedOptionResult> results(parsed.size());

        py::gil_scoped_release release;

#if defined(_OPENMP)
        const Size pathWorkers = buehlerMcPathWorkersFromEnvironment();
        buehlerMcApplyOpenMpTuning();
        omp_set_num_threads(static_cast<int>(pathWorkers));
        // dynamic: per-option cost is heterogeneous (asian/barrier vs european) and books
        // are often sorted by product, which starves static chunking.
#pragma omp parallel for schedule(dynamic)
#endif
        for (std::ptrdiff_t i = 0; i < static_cast<std::ptrdiff_t>(parsed.size()); ++i) {
            PricedOptionResult row;
            row.id = parsed[static_cast<Size>(i)].id;
            try {
                const BuehlerMcPathPricingResult priced =
                    priceSpecOnSavePath(parsed[static_cast<Size>(i)], bank, model);
                row.value = static_cast<double>(priced.value);
                row.stderr = static_cast<double>(priced.errorEstimate);
                row.status = "ok";
            } catch (const std::exception& ex) {
                row.status = ex.what();
            }
            results[static_cast<Size>(i)] = std::move(row);
        }
        return results;
    }

    /**
     * Simulate-and-price across sequential MC sub-banks (<= 50k paths each): only one bank
     * is resident at a time (peak RAM = one sub-bank) and each option's stderr is the honest
     * dispersion of independent sub-bank prices instead of the optimistic single-bank
     * estimate that ignores leverage-binning correlation. The stored bank is cleared at the
     * end; call simulate_paths() again if price_all() on a persistent bank is needed.
     */
    std::vector<PricedOptionResult> price_book(const py::list& specs,
                                               const std::string& expiry_iso,
                                               const Size mc_samples, const BigNatural seed,
                                               const std::string& dynamics,
                                               const std::vector<std::string>& save_fixing_dates,
                                               const Size subbank_samples, const Size lsv_bins,
                                               const Size path_workers) {
        const Date expiry = parseIsoDate(expiry_iso);
        Size nSubbanks = 0;
        Size subbankSamples = 0;
        if (subbank_samples > 0) {
            // Explicit sub-bank size: peak RAM scales with it; smaller values trade
            // simulation overhead for more independent stderr samples.
            subbankSamples = std::min<Size>(subbank_samples, mc_samples);
            nSubbanks = (mc_samples + subbankSamples - 1) / subbankSamples;
        } else {
            notebookMcSubbankLayout(mc_samples, nSubbanks, subbankSamples);
        }

        BuehlerMcSettings settings =
            makeMcSettings(subbankSamples, seed, dynamics, save_fixing_dates, lsv_bins,
                           path_workers);

        const Real spot = market_data_->spotValue();
        std::vector<ParsedOptionSpec> parsed;
        parsed.reserve(specs.size());
        for (const py::handle item : specs) {
            parsed.push_back(parseOptionSpec(py::cast<py::dict>(item), spot,
                                             market_data_->today(), market_data_->calendar()));
        }

        std::vector<McSubbankAccumulator> accumulators(parsed.size());
        std::vector<std::string> failure(parsed.size());
        const BuehlerModel& model = *model_;

        py::gil_scoped_release release;

        for (Size subbank = 0; subbank < nSubbanks; ++subbank) {
            // Same seed spacing as the C++ verify grids; sub-bank streams stay disjoint.
            settings.seed = seed + static_cast<BigNatural>(subbank) * 1000003ULL;
            model_->simulateFixingPaths(expiry, {}, settings);
            const BuehlerFixingSavePath& bank = model_->fixingSavePath();

#if defined(_OPENMP)
            const Size pathWorkers = buehlerMcPathWorkersFromEnvironment();
            buehlerMcApplyOpenMpTuning();
            omp_set_num_threads(static_cast<int>(pathWorkers));
#pragma omp parallel for schedule(dynamic)
#endif
            for (std::ptrdiff_t i = 0; i < static_cast<std::ptrdiff_t>(parsed.size()); ++i) {
                const Size idx = static_cast<Size>(i);
                if (!failure[idx].empty()) {
                    continue;
                }
                try {
                    accumulators[idx].add(priceSpecOnSavePath(parsed[idx], bank, model));
                } catch (const std::exception& ex) {
                    failure[idx] = ex.what();
                }
            }
        }
        model_->takeFixingSavePath(); // free the last bank; price_book owns no state

        std::vector<PricedOptionResult> results(parsed.size());
        for (Size i = 0; i < parsed.size(); ++i) {
            PricedOptionResult row;
            row.id = parsed[i].id;
            if (!failure[i].empty()) {
                row.status = failure[i];
            } else {
                const BuehlerMcPathPricingResult priced = accumulators[i].finish();
                row.value = static_cast<double>(priced.value);
                row.stderr = static_cast<double>(priced.errorEstimate);
                row.status = "ok";
            }
            results[i] = std::move(row);
        }
        return results;
    }

    /** Same rule as option specs: asof + round(365*T) days, adjusted TARGET Following. */
    std::string resolve_expiry_years(const double years) const {
        if (years <= 0.0) {
            throw std::invalid_argument("expiry_years must be positive");
        }
        const Date nominal =
            market_data_->today() + static_cast<Integer>(std::lround(years * 365.0));
        return formatIsoDate(market_data_->calendar().adjust(nominal, Following));
    }

    std::string today() const { return formatIsoDate(model_->today()); }

    double spot() const { return static_cast<double>(market_data_->spotValue()); }

private:
    std::unique_ptr<MarketData> market_data_;
    std::unique_ptr<BuehlerModel> model_;

    void rebuild_model() { model_ = std::make_unique<BuehlerModel>(*market_data_); }
};

} // namespace

PYBIND11_MODULE(pricing_engine, m) {
    m.doc() = "Buehler LV/LSV pricing engine (QuantLib)";

    py::class_<BuehlerCalibrationValidationOptions>(m, "ValidateOptions")
        .def(py::init<>())
        .def_readwrite("mean_iv_err_bp_threshold",
                       &BuehlerCalibrationValidationOptions::meanIvErrBpThreshold)
        .def_readwrite("fd_t_grid_per_year", &BuehlerCalibrationValidationOptions::fdTGridPerYear)
        .def_readwrite("fd_x_grid", &BuehlerCalibrationValidationOptions::fdXGrid)
        .def_readwrite("throw_on_failure", &BuehlerCalibrationValidationOptions::throwOnFailure)
        .def_readwrite("verbose", &BuehlerCalibrationValidationOptions::verbose);

    py::class_<BuehlerCalibrationSmileFitSample>(m, "SmileFitSample")
        .def_property_readonly("expiry", [](const BuehlerCalibrationSmileFitSample& s) {
            return formatIsoDate(s.expiry);
        })
        .def_readonly("strike_s", &BuehlerCalibrationSmileFitSample::strikeS)
        .def_readonly("lv_price_s", &BuehlerCalibrationSmileFitSample::lvPriceS)
        .def_readonly("sigma_market_s", &BuehlerCalibrationSmileFitSample::sigmaMarketS)
        .def_readonly("sigma_imp_s", &BuehlerCalibrationSmileFitSample::sigmaImpS)
        .def_readonly("abs_err_iv_bp", &BuehlerCalibrationSmileFitSample::absErrIvBp)
        .def_property_readonly("status", [](const BuehlerCalibrationSmileFitSample& s) {
            return std::string(s.status);
        });

    py::class_<BuehlerCalibrationValidationReport>(m, "ValidationReport")
        .def_readonly("static_arbitrage_ok", &BuehlerCalibrationValidationReport::staticArbitrageOk)
        .def_readonly("smile_fit_ok", &BuehlerCalibrationValidationReport::smileFitOk)
        .def_readonly("mean_abs_iv_err_bp", &BuehlerCalibrationValidationReport::meanAbsIvErrBp)
        .def_readonly("smile_fit_samples", &BuehlerCalibrationValidationReport::smileFitSamples)
        .def_readonly("expected_smile_fit_samples",
                      &BuehlerCalibrationValidationReport::expectedSmileFitSamples)
        .def_readonly("smile_fit_cells", &BuehlerCalibrationValidationReport::smileFitCells)
        .def("passed", &BuehlerCalibrationValidationReport::passed);

    py::class_<BuehlerImpliedVolXArbitrageReport>(m, "ArbitrageReport")
        .def_readonly("violations_butterfly",
                      &BuehlerImpliedVolXArbitrageReport::violationsButterfly)
        .def_readonly("violations_calendar", &BuehlerImpliedVolXArbitrageReport::violationsCalendar)
        .def_readonly("min_butterfly", &BuehlerImpliedVolXArbitrageReport::minButterfly)
        .def_readonly("min_calendar", &BuehlerImpliedVolXArbitrageReport::minCalendar)
        .def("all_passed", &BuehlerImpliedVolXArbitrageReport::allPassed);

    py::class_<BuehlerMcPathPricingResult>(m, "McPriceResult")
        .def_readonly("value", &BuehlerMcPathPricingResult::value)
        .def_readonly("stderr", &BuehlerMcPathPricingResult::errorEstimate);

    py::class_<PricedOptionResult>(m, "PricedOption")
        .def_readonly("id", &PricedOptionResult::id)
        .def_readonly("value", &PricedOptionResult::value)
        .def_readonly("stderr", &PricedOptionResult::stderr)
        .def_readonly("status", &PricedOptionResult::status);

    py::class_<LvIvFitRow>(m, "LvIvFitRow")
        .def_property_readonly("expiry",
                               [](const LvIvFitRow& row) { return formatIsoDate(row.expiry); })
        .def_readonly("strike_s", &LvIvFitRow::strikeS)
        .def_readonly("tenor_years", &LvIvFitRow::tenorYears)
        .def_readonly("sigma_market_s", &LvIvFitRow::sigmaMarketS)
        .def_readonly("sigma_lv_s", &LvIvFitRow::sigmaLvS)
        .def_readonly("abs_err_iv_bp", &LvIvFitRow::absErrIvBp);

    py::class_<LvFixedXMarketRow>(m, "LvFixedXMarketRow")
        .def_property_readonly("expiry",
                               [](const LvFixedXMarketRow& row) { return formatIsoDate(row.expiry); })
        .def_readonly("strike_s", &LvFixedXMarketRow::strikeS)
        .def_readonly("tenor_years", &LvFixedXMarketRow::tenorYears)
        .def_readonly("kx", &LvFixedXMarketRow::kx)
        .def_readonly("sigma_loc_x", &LvFixedXMarketRow::sigmaLocX);

    py::class_<LvFixedXMarketGrid>(m, "LvFixedXMarketGrid")
        .def_readonly("rows", &LvFixedXMarketGrid::rows)
        .def_readonly("market_t_min", &LvFixedXMarketGrid::marketTMin)
        .def_readonly("market_t_max", &LvFixedXMarketGrid::marketTMax)
        .def_readonly("market_kx_min", &LvFixedXMarketGrid::marketKxMin)
        .def_readonly("market_kx_max", &LvFixedXMarketGrid::marketKxMax)
        .def_readonly("tabulated_kx_min", &LvFixedXMarketGrid::tabulatedKxMin)
        .def_readonly("tabulated_kx_max", &LvFixedXMarketGrid::tabulatedKxMax)
        .def_readonly("tabulated_t_min", &LvFixedXMarketGrid::tabulatedTMin)
        .def_readonly("tabulated_t_max", &LvFixedXMarketGrid::tabulatedTMax);

    py::class_<LvFixedXTabulatedGrid>(m, "LvFixedXTabulatedGrid")
        .def_readonly("tenor_years", &LvFixedXTabulatedGrid::tenorYears)
        .def_readonly("kx", &LvFixedXTabulatedGrid::kx)
        .def_readonly("sigma", &LvFixedXTabulatedGrid::sigma);

    py::class_<LsvVsLvRow>(m, "LsvVsLvRow")
        .def_property_readonly("expiry",
                               [](const LsvVsLvRow& row) { return formatIsoDate(row.expiry); })
        .def_readonly("strike_s", &LsvVsLvRow::strikeS)
        .def_readonly("lv_price_s", &LsvVsLvRow::lvPriceS)
        .def_readonly("lsv_price_s", &LsvVsLvRow::lsvPriceS)
        .def_readonly("iv_lv_s", &LsvVsLvRow::ivLvS)
        .def_readonly("iv_lsv_s", &LsvVsLvRow::ivLsvS)
        .def_readonly("abs_err_iv_bp", &LsvVsLvRow::absErrIvBp);

    py::class_<PricingContext::MarketSummary>(m, "MarketSummary")
        .def_readonly("asof", &PricingContext::MarketSummary::asof)
        .def_readonly("spot", &PricingContext::MarketSummary::spot)
        .def_readonly("expiries", &PricingContext::MarketSummary::expiries)
        .def_readonly("expiry_years", &PricingContext::MarketSummary::expiry_years)
        .def_readonly("strikes", &PricingContext::MarketSummary::strikes)
        .def_readonly("implied_vols", &PricingContext::MarketSummary::implied_vols)
        .def_readonly("dividend_dates", &PricingContext::MarketSummary::dividend_dates)
        .def_readonly("dividend_amounts", &PricingContext::MarketSummary::dividend_amounts)
        .def_readonly("rfr_tenor_years", &PricingContext::MarketSummary::rfr_tenor_years)
        .def_readonly("rfr_zero_rates", &PricingContext::MarketSummary::rfr_zero_rates)
        .def_readonly("repo_tenor_years", &PricingContext::MarketSummary::repo_tenor_years)
        .def_readonly("repo_zero_rates", &PricingContext::MarketSummary::repo_zero_rates)
        .def_readonly("bergomi_k", &PricingContext::MarketSummary::bergomi_k)
        .def_readonly("bergomi_nu", &PricingContext::MarketSummary::bergomi_nu)
        .def_readonly("bergomi_rho", &PricingContext::MarketSummary::bergomi_rho);

    py::class_<PricingContext::SimulateResult>(m, "SimulateResult")
        .def_readonly("sim_ms", &PricingContext::SimulateResult::sim_ms)
        .def_readonly("expiry", &PricingContext::SimulateResult::expiry)
        .def_readonly("mc_samples", &PricingContext::SimulateResult::mc_samples)
        .def_readonly("dynamics", &PricingContext::SimulateResult::dynamics);

    py::class_<PricingContext>(m, "PricingContext")
        .def_static("from_tables", &PricingContext::from_tables, py::arg("asof"), py::arg("spot"),
                    py::arg("rfr_tenor_years"), py::arg("rfr_zero_rates"),
                    py::arg("repo_tenor_years"), py::arg("repo_zero_rates"),
                    py::arg("vol_tenor_years"), py::arg("vol_strikes"), py::arg("implied_vols"),
                    py::arg("dividend_dates") = std::vector<std::string>{},
                    py::arg("dividend_amounts") = std::vector<double>{},
                    py::arg("bergomi_k") = 2.0, py::arg("bergomi_nu") = 1.0,
                    py::arg("bergomi_rho") = -0.7)
        .def_static("from_sample", &PricingContext::from_sample)
        .def_static("from_mock", &PricingContext::from_mock)
        .def("preprocessing", &PricingContext::preprocessing)
        .def("calibration", &PricingContext::calibration, py::arg("run_validation") = true)
        .def("validate_calibration",
             [](PricingContext& self, BuehlerCalibrationValidationOptions options) {
                 return self.validate_calibration(options);
             },
             py::arg("options") = BuehlerCalibrationValidationOptions{})
        .def("check_static_arbitrage", &PricingContext::check_static_arbitrage,
             py::arg("n_time_samples") = 32, py::arg("n_strike_samples") = 64,
             py::arg("tol_butterfly") = -5.0e-4, py::arg("tol_calendar") = -5.0e-4)
        .def("simulate_paths", &PricingContext::simulate_paths, py::arg("expiry_iso"),
             py::arg("mc_samples") = kDefaultMcSamples, py::arg("seed") = kDefaultMcSeed,
             py::arg("dynamics") = "lsv",
             py::arg("save_fixing_dates") = std::vector<std::string>{},
             py::arg("lsv_bins") = 0, py::arg("path_workers") = 0,
             py::call_guard<py::gil_scoped_release>())
        .def("simulate_paths_near_years", &PricingContext::simulate_paths_near_years,
             py::arg("horizon_years"), py::arg("mc_samples") = kDefaultMcSamples,
             py::arg("seed") = kDefaultMcSeed, py::arg("dynamics") = "lsv",
             py::arg("save_fixing_dates") = std::vector<std::string>{},
             py::arg("lsv_bins") = 0, py::arg("path_workers") = 0,
             py::call_guard<py::gil_scoped_release>())
        .def("market_summary", &PricingContext::market_summary)
        .def("price_european_fd", &PricingContext::price_european_fd, py::arg("expiry_iso"),
             py::arg("strike_fraction_of_spot") = 1.0, py::arg("is_call") = true,
             py::arg("quote_space") = "S", py::arg("t_grid_per_year") = kDefaultFdTGridPerYear,
             py::arg("x_grid") = kDefaultFdXGrid)
        .def("price_digital_fd", &PricingContext::price_digital_fd, py::arg("expiry_iso"),
             py::arg("strike_fraction_of_spot") = 1.0, py::arg("asset_or_nothing") = false,
             py::arg("quote_space") = "S", py::arg("t_grid_per_year") = kDefaultFdTGridPerYear,
             py::arg("x_grid") = kDefaultFdXGrid)
        .def("verify_lv_bs_consistency", &PricingContext::verify_lv_bs_consistency,
             py::arg("t_grid_per_year") = kDefaultFdTGridPerYear,
             py::arg("x_grid") = kDefaultFdXGrid)
        .def("export_lv_fixed_x_csv", &PricingContext::export_lv_fixed_x_csv,
             py::arg("output_path") = "build-std/lv_fixed_x.csv")
        .def("fixed_lv_x_market_grid", &PricingContext::fixed_lv_x_market_grid)
        .def("fixed_lv_x_tabulated_grid", &PricingContext::fixed_lv_x_tabulated_grid)
        .def("verify_lsv_mc_vs_lv_fd", &PricingContext::verify_lsv_mc_vs_lv_fd,
             py::arg("mc_samples") = kDefaultMcSamples,
             py::arg("t_grid_per_year") = kDefaultFdTGridPerYear,
             py::arg("x_grid") = kDefaultFdXGrid, py::call_guard<py::gil_scoped_release>())
        .def("price_all", &PricingContext::price_all, py::arg("specs"))
        .def("price_book", &PricingContext::price_book, py::arg("specs"), py::arg("expiry_iso"),
             py::arg("mc_samples") = kDefaultMcSamples, py::arg("seed") = kDefaultMcSeed,
             py::arg("dynamics") = "lsv",
             py::arg("save_fixing_dates") = std::vector<std::string>{},
             py::arg("subbank_samples") = 0, py::arg("lsv_bins") = 0,
             py::arg("path_workers") = 0)
        .def("resolve_expiry_years", &PricingContext::resolve_expiry_years, py::arg("years"))
        .def_property_readonly("today", &PricingContext::today)
        .def_property_readonly("spot", &PricingContext::spot);
}
