/**
 * @file buehler_model.h
 * @brief Buehler pure-X calibration: implied vol, fixed local vol, optional MC bank.
 */

#ifndef BUEHLER_MODEL_H
#define BUEHLER_MODEL_H

#include "buehler_fixing_save_path.h"
#include "buehler_mc_settings.h"
#include <ql/quantlib.hpp>
#include <memory>
#include <optional>
#include <vector>

class MarketData;
class BuehlerMcTimeGridSigmaLookup;

/** Dupire repair health gates: fraction of dense-grid cells repaired by copying the
 *  nearest good LV at larger kx (IV only if no such donor). Above warn → stderr;
 *  above fail → throw. */
constexpr double kDupireRepairWarnFraction = 0.05;
constexpr double kDupireRepairFailFraction = 0.25;

/** @brief Dupire dense-grid repair counts from the last calibration. */
struct BuehlerLvDenseRepairCounts {
    QuantLib::Size denseGridCells = 0;
    /** Cells filled from a larger-kx good LV, or last-resort IV if no donor. */
    QuantLib::Size dupireRepairs = 0;
};

/** @brief Bergomi 1-factor OU driver for LSV dynamics on the pure-stock coordinate @e X. */
struct BuehlerBergomiParams {
    QuantLib::Real k = 0.0;
    QuantLib::Real nu = 0.0;
    QuantLib::Real rho = 0.0;
};

/** @brief Options for @c BuehlerModel::validate_calibration. */
struct BuehlerCalibrationValidationOptions {
    bool throwOnFailure = true;
    /** Print the PASS/FAIL summary to stdout (library callers usually read the report instead). */
    bool verbose = false;
};

/** @brief Result of @c BuehlerModel::validate_calibration (static arb on σ_X). */
struct BuehlerCalibrationValidationReport {
    bool staticArbitrageOk = false;
    bool passed() const { return staticArbitrageOk; }
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
     * @brief Post-calibration gate: butterfly+calendar static arb on σ_X.
     * Throws if @p options.throwOnFailure and the check fails.
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
    const QuantLib::Handle<QuantLib::BlackVolTermStructure>& impliedVolXTs() const {
        return impliedVolXTs_;
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
    QuantLib::Size lastLvDupireRepairs() const { return lastLvDenseRepair_.dupireRepairs; }

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

    const std::vector<QuantLib::Date>& nodalImpliedVolXExpiries() const {
        return nodalImpliedVolXExpiries_;
    }
    const std::vector<QuantLib::Real>& nodalImpliedVolXKxGrid() const {
        return nodalImpliedVolXKxGrid_;
    }
    /** @brief Nodal σ_X before surface wrap, indexed [kx][expiry]. */
    const QuantLib::Matrix& nodalImpliedVolsX() const { return nodalImpliedVolsX_; }

    /**
     * @brief Lower reliable kx mark \(\max_T k_x(K_{\min},T)\).
     * Valid after @c calibration(); verify skips pillars with kx <= this
     * (incomplete cross-expiry market support on the wide LV axis, including
     * the expiry that attains the max).
     */
    QuantLib::Real calibrationMinKx() const;

    /**
     * @brief Upper reliable kx mark \(\min_T k_x(K_{\max},T)\).
     * Valid after @c calibration().
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
    QuantLib::Handle<QuantLib::BlackVolTermStructure> impliedVolXTs_;
    QuantLib::Handle<QuantLib::LocalVolTermStructure> fixedPureLocalVolTs_;
    QuantLib::Matrix denseLocalVolXGrid_;

    BuehlerLvDenseRepairCounts lastLvDenseRepair_;

    std::optional<BuehlerBergomiParams> bergomiParams_;

    std::vector<QuantLib::Date> nodalImpliedVolXExpiries_;
    std::vector<QuantLib::Real> nodalImpliedVolXKxGrid_;
    QuantLib::Matrix nodalImpliedVolsX_;
    QuantLib::Real calibrationMinKx_ = QuantLib::Null<QuantLib::Real>();
    QuantLib::Real calibrationMaxKx_ = QuantLib::Null<QuantLib::Real>();

    std::optional<BuehlerFixingSavePath> fixingSavePath_;
    QuantLib::Date fixingPathHorizonMax_;
    std::vector<QuantLib::Date> fixingPathSimulationDates_;

    /** Lazily built by @c mcSigmaLookup(); reset by @c calibration(). */
    mutable std::shared_ptr<const BuehlerMcTimeGridSigmaLookup> mcSigmaLookupCache_;
};

#endif
