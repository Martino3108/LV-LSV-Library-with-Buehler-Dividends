/**
 * @file buehler_model.h
 * @brief Buehler pure-X calibration: implied vol, fixed local vol, optional MC bank.
 */

#ifndef BUEHLER_MODEL_H
#define BUEHLER_MODEL_H

#include "buehler_fixing_save_path.h"
#include "buehler_mc_settings.h"
#include "fd_buehler_x_fdm.h"
#include <ql/quantlib.hpp>
#include <memory>
#include <optional>
#include <vector>

class MarketData;
class BuehlerMcTimeGridSigmaLookup;

/** Mean |Δσ| gate (bp) for @c validate_calibration on the 3×3 FD smile-fit grid. */
constexpr double kDefaultValidateMeanIvErrBpThreshold = 50.0;

/** Dupire repair health gates: fraction of dense-grid cells that fell back to the Black
 *  vol proxy. Above the warn level, calibration prints a stderr warning; above the fail
 *  level, calibration throws (the local vol no longer reprices the surface it was built from). */
constexpr double kDupireBlackFallbackWarnFraction = 0.05;
constexpr double kDupireBlackFallbackFailFraction = 0.25;

/** @brief Dupire dense-grid repair counts from the last calibration. */
struct BuehlerLvDenseRepairCounts {
    QuantLib::Size denseGridCells = 0;
    QuantLib::Size dupireBlackFallbacks = 0;
};

/** @brief Bergomi 1-factor OU driver for LSV dynamics on the pure-stock coordinate @e X. */
struct BuehlerBergomiParams {
    QuantLib::Real k = 0.0;
    QuantLib::Real nu = 0.0;
    QuantLib::Real rho = 0.0;
};

/** @brief One cell of the 3×3 FD vs market IV smile-fit grid in @c validate_calibration. */
struct BuehlerCalibrationSmileFitSample {
    QuantLib::Date expiry;
    QuantLib::Real strikeS = QuantLib::Null<QuantLib::Real>();
    QuantLib::Real lvPriceS = QuantLib::Null<QuantLib::Real>();
    QuantLib::Real sigmaMarketS = QuantLib::Null<QuantLib::Real>();
    QuantLib::Real sigmaImpS = QuantLib::Null<QuantLib::Real>();
    double absErrIvBp = 0.0;
    /** @c ok, @c fd_failed, @c lv_price_too_low, @c bad_market_vol, @c inversion_failed, @c skipped */
    const char* status = "skipped";
};

/** @brief Options for @c BuehlerModel::validate_calibration. */
struct BuehlerCalibrationValidationOptions {
    /** Mean |Δσ| gate (bp) on the 3×3 FD fit grid (wings + term extremes). */
    double meanIvErrBpThreshold = kDefaultValidateMeanIvErrBpThreshold;
    QuantLib::Size fdTGridPerYear = kDefaultFdTGridPerYear;
    QuantLib::Size fdXGrid = kDefaultFdXGrid;
    bool throwOnFailure = true;
    /** Print the PASS/FAIL summary to stdout (library callers usually read the report instead). */
    bool verbose = false;
};

/** @brief Result of @c BuehlerModel::validate_calibration (arb + FD smile fit on 3 expiries). */
struct BuehlerCalibrationValidationReport {
    bool staticArbitrageOk = false;
    bool smileFitOk = false;
    double meanAbsIvErrBp = 0.0;
    QuantLib::Size smileFitSamples = 0;
    QuantLib::Size expectedSmileFitSamples = 0;
    QuantLib::Size priceableStrikeCount = 0;
    QuantLib::Date probeShortestExpiry;
    std::vector<BuehlerCalibrationSmileFitSample> smileFitCells;
    bool passed() const { return staticArbitrageOk && smileFitOk; }
};

/** @brief Pure-X surfaces and fixed LV built from @ref MarketData. */
class BuehlerModel {
private:
    std::vector<QuantLib::Date> denseExpiries_;
    std::vector<QuantLib::Real> denseXStrikes_;

public:
    const std::vector<QuantLib::Date>& denseExpiries() const { return denseExpiries_; }
    const std::vector<QuantLib::Real>& denseXStrikes() const { return denseXStrikes_; }

    explicit BuehlerModel(MarketData& marketData);
    explicit BuehlerModel(const MarketData& marketData);

    void preprocessing();
    /** @brief LV calibration; runs @c validate_calibration() when @p runValidation is true (default). */
    void calibration(bool runValidation = true);

    /**
     * @brief Post-calibration gate: butterfly+calendar static arb on σ_X bicubic, then mean IV fit.
     * Smile fit uses @c LvEuropeanFdBuehlerOption (FD) on a 3×3 grid: 2nd, middle, and last
     * market expiries and strikes (earliest pillar/strike skipped). Uses the market snapshot
     * copied at construction. Throws if @p options.throwOnFailure and a check fails.
     * Requires @c preprocessing() and @c calibration() first.
     */
    BuehlerCalibrationValidationReport validate_calibration(
        BuehlerCalibrationValidationOptions options = {}) const;

    const QuantLib::Date& today() const { return today_; }
    const QuantLib::Date& maturity() const { return maturity_; }
    const QuantLib::Calendar& calendar() const { return calendar_; }
    const QuantLib::DayCounter& dayCounter() const { return dayCounter_; }

    /**
     * @brief Calendar-day nodes used for the affine map A(t), D(t) after @c preprocessing()
     * (every calendar day from today through maturity; ACT/365-aligned).
     */
    const std::vector<QuantLib::Date>& businessDates() const { return businessDates_; }
    const std::vector<QuantLib::Real>& forwards0T() const { return forwards0T_; }
    const std::vector<QuantLib::Real>& dividends0T() const { return dividends0T_; }
    const std::vector<QuantLib::Real>& pureSlopes() const { return pureSlopes_; }
    const std::vector<QuantLib::Real>& pureIntercepts() const { return pureIntercepts_; }

    QuantLib::Real forward0T(const QuantLib::Date& t) const;
    QuantLib::Real dividendCarry0T(const QuantLib::Date& t) const;
    /** @brief Affine map: S = a(T)·x + b(T). */
    QuantLib::Real mapXtoS(const QuantLib::Date& t, QuantLib::Real x) const;

    const QuantLib::Handle<QuantLib::YieldTermStructure>& riskFreeTs() const {
        return inputRiskFreeTs_;
    }
    const QuantLib::Handle<QuantLib::YieldTermStructure>& repoTs() const { return inputRepoTs_; }

    QuantLib::Real snapshotSpot() const { return inputSpotValue_; }

    const std::vector<QuantLib::Date>& snapshotRiskFreeDates() const { return inputRiskFreeDates_; }
    const std::vector<QuantLib::Rate>& snapshotRiskFreeZeros() const { return inputRiskFreeZeroRates_; }
    const std::vector<QuantLib::Date>& snapshotRepoDates() const { return inputRepoDates_; }
    const std::vector<QuantLib::Rate>& snapshotRepoZeros() const { return inputRepoZeroRates_; }
    const std::vector<QuantLib::Date>& snapshotDividendExDates() const { return inputDividendDates_; }
    const std::vector<QuantLib::Real>& snapshotDividendCash() const { return inputDividendAmounts_; }
    const std::vector<QuantLib::Real>& snapshotDividendProportional() const {
        return inputDividendProportional_;
    }
    const std::vector<QuantLib::Real>& snapshotMarketStrikes() const { return inputStrikes_; }
    const std::vector<QuantLib::Date>& snapshotMarketExpiries() const { return inputExpiries_; }
    const QuantLib::Matrix& snapshotMarketImpliedVols() const { return inputImpliedVolsMarketS_; }

    const QuantLib::Handle<QuantLib::BlackVolTermStructure>& pureBlackVolTs() const {
        return pureBlackVolTs_;
    }
    const QuantLib::Handle<QuantLib::BlackVolTermStructure>& impliedVolXBicubicTs() const {
        return impliedVolXBicubicTs_;
    }
    /** @brief Fixed Dupire LV in X (use for FD / MC). */
    const QuantLib::Handle<QuantLib::LocalVolTermStructure>& fixedPureLocalVolTs() const {
        return fixedPureLocalVolTs_;
    }

    /**
     * @brief Tabulated σ(t,X) MC lookup on the dense calib grid; built lazily, cached
     * until the next @c calibration() (the table only depends on the fixed LV surface).
     */
    const BuehlerMcTimeGridSigmaLookup& mcSigmaLookup() const;

    /** @brief Dense Dupire grid passed to @c FixedLocalVolSurface. */
    const QuantLib::Matrix& denseLocalVolXGrid() const { return denseLocalVolXGrid_; }

    const BuehlerLvDenseRepairCounts& lastLvDenseRepairCounts() const { return lastLvDenseRepair_; }
    QuantLib::Size lastLvDupireBlackFallbacks() const { return lastLvDenseRepair_.dupireBlackFallbacks; }

    bool hasLsvCalibration() const { return bergomiParams_.has_value(); }

    /** @brief Bergomi 1F driver; model input copied from @ref MarketData at construction. */
    const BuehlerBergomiParams& bergomiParams() const;
    /** @brief Override the Bergomi inputs (e.g. scenario bumps); validated. */
    void setBergomiParams(const BuehlerBergomiParams& params);

    /**
     * @brief Simulate fixing bank up to @p horizonMax.
     * @param horizonMax Last simulation date (inclusive).
     * @param simulationDates Empty → every calendar day to horizon (ACT/365), then unioned with
     *        @c settings.mcSavePathFixingDates. Cleared by @c calibration().
     * @param settings MC dynamics, samples, fast path, LSV bins, etc.
     */
    void simulateFixingPaths(const QuantLib::Date& horizonMax,
                             const std::vector<QuantLib::Date>& simulationDates = {},
                             const BuehlerMcSettings& settings = BuehlerMcSettings{});

    /**
     * @brief Const variant of @c simulateFixingPaths: returns the bank instead of storing it
     * on the model, so a calibrated model can be shared read-only (e.g. across sequential
     * sub-bank pricing loops) without mutating state or double-holding banks.
     */
    BuehlerFixingSavePath simulateFixingBank(const QuantLib::Date& horizonMax,
                                             const std::vector<QuantLib::Date>& simulationDates = {},
                                             const BuehlerMcSettings& settings = BuehlerMcSettings{}) const;

    bool hasFixingSavePath() const { return fixingSavePath_.has_value(); }
    const BuehlerFixingSavePath& fixingSavePath() const;
    /** @brief Move the stored save path out (clears @c hasFixingSavePath()). */
    BuehlerFixingSavePath takeFixingSavePath();
    const QuantLib::Date& fixingPathHorizonMax() const;
    const std::vector<QuantLib::Date>& fixingPathSimulationDates() const;

    const std::vector<QuantLib::Date>& preBicubicImpliedVolXExpiries() const {
        return preBicubicImpliedVolXExpiries_;
    }
    const std::vector<QuantLib::Real>& preBicubicImpliedVolXKxGrid() const {
        return preBicubicImpliedVolXKxGrid_;
    }
    /** @brief Nodal σ_X before bicubic, indexed [kx][expiry]. */
    const QuantLib::Matrix& preBicubicImpliedVolsX() const { return preBicubicImpliedVolsX_; }

    /**
     * @brief Lower kx bound used for LV calibration (injected K_min node,
     * \(\max_T k_x(K_{\min},T)\)).
     * Valid after @c calibration(); pillars with kx below this are excluded from fit.
     */
    QuantLib::Real calibrationMinKx() const;

    /**
     * @brief Upper kx bound used for LV calibration (injected K_max node,
     * \(\min_T k_x(K_{\max},T)\)).
     * Valid after @c calibration(); pillars with kx above this are excluded from fit.
     */
    QuantLib::Real calibrationMaxKx() const;

private:
    QuantLib::Real interpolateByDate(const std::vector<QuantLib::Real>& values,
                                     const QuantLib::Date& t) const;

    QuantLib::Real inputSpotValue_ = 0.0;
    QuantLib::Handle<QuantLib::YieldTermStructure> inputRiskFreeTs_;
    QuantLib::Handle<QuantLib::YieldTermStructure> inputRepoTs_;
    QuantLib::Handle<QuantLib::BlackVolTermStructure> inputBlackVolTs_;
    std::vector<QuantLib::Date> inputDividendDates_;
    std::vector<QuantLib::Real> inputDividendAmounts_;
    std::vector<QuantLib::Real> inputDividendProportional_;
    std::vector<QuantLib::Real> inputStrikes_;
    std::vector<QuantLib::Date> inputExpiries_;
    std::vector<QuantLib::Date> inputRiskFreeDates_;
    std::vector<QuantLib::Rate> inputRiskFreeZeroRates_;
    std::vector<QuantLib::Date> inputRepoDates_;
    std::vector<QuantLib::Rate> inputRepoZeroRates_;
    QuantLib::Matrix inputImpliedVolsMarketS_;

    QuantLib::Date today_;
    QuantLib::Date maturity_;
    QuantLib::Calendar calendar_;
    QuantLib::DayCounter dayCounter_;

    std::vector<QuantLib::Date> businessDates_;
    std::vector<QuantLib::Time> businessTimes_;
    std::vector<QuantLib::Real> forwards0T_;
    std::vector<QuantLib::Real> dividends0T_;
    std::vector<QuantLib::Real> pureSlopes_;
    std::vector<QuantLib::Real> pureIntercepts_;

    QuantLib::Handle<QuantLib::BlackVolTermStructure> pureBlackVolTs_;
    QuantLib::Handle<QuantLib::BlackVolTermStructure> impliedVolXBicubicTs_;
    QuantLib::Handle<QuantLib::LocalVolTermStructure> fixedPureLocalVolTs_;
    QuantLib::Matrix denseLocalVolXGrid_;

    BuehlerLvDenseRepairCounts lastLvDenseRepair_;

    std::optional<BuehlerBergomiParams> bergomiParams_;

    std::vector<QuantLib::Date> preBicubicImpliedVolXExpiries_;
    std::vector<QuantLib::Real> preBicubicImpliedVolXKxGrid_;
    QuantLib::Matrix preBicubicImpliedVolsX_;
    QuantLib::Real calibrationMinKx_ = QuantLib::Null<QuantLib::Real>();
    QuantLib::Real calibrationMaxKx_ = QuantLib::Null<QuantLib::Real>();

    std::optional<BuehlerFixingSavePath> fixingSavePath_;
    QuantLib::Date fixingPathHorizonMax_;
    std::vector<QuantLib::Date> fixingPathSimulationDates_;

    /** Lazily built by @c mcSigmaLookup(); reset by @c calibration(). */
    mutable std::shared_ptr<const BuehlerMcTimeGridSigmaLookup> mcSigmaLookupCache_;
};

#endif
