/**
 * @file verify_fit.h
 * @brief LV/LSV fit checks: FD vs Black in X and LSV MC vs LV FD in S.
 */

#ifndef VERIFY_FIT_H
#define VERIFY_FIT_H

#include "buehler_mc_settings.h"
#include "buehler_model.h"
#include "fd_buehler_x_fdm.h"
#include <ql/quantlib.hpp>
#include <string>

class MarketData;

/**
 * @brief FD on fixed LV in X vs analytic Black.
 * @param md Market dataset used to build @p buehler.
 * @param buehler Calibrated model (fixed pure-X LV).
 * Logs to @c out_verify.txt and writes @c build-std/lv_fixed_x.csv.
 */
void verify_LV_BS_consistency(
    const MarketData& md,
    const BuehlerModel& buehler,
    QuantLib::Size tGridPerYear = kDefaultFdTGridPerYear,
    QuantLib::Size xGrid = kDefaultFdXGrid,
    bool verbose = true);

/**
 * @brief LSV MC vs LV FD in S on the same grid as @c verify_LV_BS_consistency.
 * @param md Market dataset; @p buehler must have @c calibration() applied (Bergomi params are model inputs).
 * @param buehler Model used for LSV simulation (mutated by MC runs).
 * Runs @p nSubbanks independent MC banks; LSVPrice = mean of sub-bank prices.
 * Logs to @c out_lsv.txt.
 */
void verify_lsv_mc_vs_lv_fd(const MarketData& md,
                            BuehlerModel& buehler,
                            QuantLib::Size nSubbanks = kDefaultLsvSubbankCount,
                            QuantLib::Size subbankSamples = kDefaultMcSamples,
                            QuantLib::Size tGridPerYear = kDefaultFdTGridPerYear,
                            QuantLib::Size xGrid = kDefaultFdXGrid,
                            bool verbose = true);

/** @brief European call in S from market Black vol on @p md. */
QuantLib::Real buehlerMarketEuropeanCallPriceInS(const MarketData& md,
                                                const BuehlerModel& buehler,
                                                const QuantLib::Date& expiry,
                                                QuantLib::Real strikeS);

/** @brief One cell of the LV FD vs market implied-vol grid (for Python plots). */
struct LvIvFitRow {
    QuantLib::Date expiry;
    QuantLib::Real strikeS = 0.0;
    QuantLib::Real tenorYears = 0.0;
    QuantLib::Real sigmaMarketS = 0.0;
    QuantLib::Real sigmaLvS = 0.0;
    QuantLib::Real absErrIvBp = 0.0;
};

/** @brief One cell of the LSV MC vs LV FD verify grid (for Python plots). */
struct LsvVsLvRow {
    QuantLib::Date expiry;
    QuantLib::Real strikeS = 0.0;
    QuantLib::Real lvPriceS = 0.0;
    QuantLib::Real lsvPriceS = 0.0;
    QuantLib::Real ivLvS = 0.0;
    QuantLib::Real ivLsvS = 0.0;
    QuantLib::Real absErrIvBp = 0.0;
};

/**
 * @brief Dense `t,kx,sigma` CSV export from calibrated @c fixedPureLocalVolTs() (500×1000 grid).
 * Optional debug export; Python plots use @c collect_lv_fixed_x_tabulated_grid() instead.
 */
void export_lv_fixed_x_csv(const BuehlerModel& buehler,
                           const std::string& output_path = "build-std/lv_fixed_x.csv",
                           bool verbose = false);

/** @brief One market pillar of fixed pure-X LV σ(t, kx) on vol-surface (T, K_S). */
struct LvFixedXMarketRow {
    QuantLib::Date expiry;
    QuantLib::Real strikeS = 0.0;
    QuantLib::Real tenorYears = 0.0;
    QuantLib::Real kx = 0.0;
    QuantLib::Real sigmaLocX = 0.0;
};

/** @brief Market vol grid samples of @c fixedPureLocalVolTs() plus tabulated-domain bounds. */
struct LvFixedXMarketGrid {
    std::vector<LvFixedXMarketRow> rows;
    QuantLib::Real marketTMin = 0.0;
    QuantLib::Real marketTMax = 0.0;
    QuantLib::Real marketKxMin = 0.0;
    QuantLib::Real marketKxMax = 0.0;
    QuantLib::Real tabulatedKxMin = 0.0;
    QuantLib::Real tabulatedKxMax = 0.0;
    QuantLib::Real tabulatedTMin = 0.0;
    QuantLib::Real tabulatedTMax = 0.0;
};

/** @brief Fixed LV in X on market (expiry × strike) pillars; uses flat kx extrapolation outside tab support. */
LvFixedXMarketGrid collect_lv_fixed_x_market_grid(const MarketData& md,
                                                  const BuehlerModel& buehler);

/** @brief Dupire tabulated fixed LV grid (@c denseXStrikes × @c denseExpiries) used by @c FixedLocalVolSurface. */
struct LvFixedXTabulatedGrid {
    std::vector<double> tenorYears;
    std::vector<double> kx;
    /** @c sigma[kxIndex][tenorIndex] = @c denseLocalVolXGrid()(kxIndex, tenorIndex). */
    std::vector<std::vector<double>> sigma;
};

LvFixedXTabulatedGrid collect_lv_fixed_x_tabulated_grid(const BuehlerModel& buehler);

/** @brief LV FD smile fit grid in S (same scan as @c verify_LV_BS_consistency IV table). */
std::vector<LvIvFitRow> collect_lv_iv_fit_grid(
    const MarketData& md,
    const BuehlerModel& buehler,
    QuantLib::Size tGridPerYear = kDefaultFdTGridPerYear,
    QuantLib::Size xGrid = kDefaultFdXGrid);

/**
 * @brief LSV MC vs LV FD grid; mutates @p buehler (MC sub-banks).
 * Same scenarios as @c verify_lsv_mc_vs_lv_fd.
 */
std::vector<LsvVsLvRow> collect_lsv_vs_lv_grid(
    const MarketData& md,
    BuehlerModel& buehler,
    QuantLib::Size nSubbanks = kDefaultLsvSubbankCount,
    QuantLib::Size subbankSamples = kDefaultMcSamples,
    QuantLib::Size tGridPerYear = kDefaultFdTGridPerYear,
    QuantLib::Size xGrid = kDefaultFdXGrid);

#endif
