/**
 * @file benchmark_bs_flat_reference.h
 * @brief Flat BS reference pricing and sanity-check table printers for pipeline_sanity.
 *
 * Several reference pricers here use QuantLib Instruments (European / discrete
 * Asian engines) which read the global @c Settings::instance().evaluationDate().
 * Callers must pin it to the snapshot date first (see pipeline_sanity_check_BS_fallback);
 * the production pricing path never depends on it.
 */

#ifndef BENCHMARK_BS_FLAT_REFERENCE_H
#define BENCHMARK_BS_FLAT_REFERENCE_H

#include "asian_mc_buehler_option.h"
#include "autocall_mc_buehler_option.h"
#include "barrier_mc_buehler_option.h"
#include "lookback_mc_buehler_option.h"
#include "bs_flat_mc_save_path.h"
#include "buehler_mc_settings.h"
#include "fd_buehler_x_fdm.h"
#include "option.h"
#include <string>
#include <vector>
#include <ql/quantlib.hpp>

class MarketData;

namespace bs_flat_reference {

constexpr QuantLib::Size kFdTGridPerYear = kDefaultFdTGridPerYear;
constexpr QuantLib::Size kFdXGrid = kDefaultFdXGrid;
constexpr double kRangeWidth = 0.02;
/** @brief Coupon per observation for autocall sanity (fraction of spot notional). */
constexpr double kAutocallSanityCouponRatePerPeriod = 0.03;

extern const std::vector<int> kAsianSanityTenorMonths;
constexpr int kAsianSavePathTenorMonths = 24;
constexpr QuantLib::Size kPipelineSanityMcSamples = kDefaultMcSamples;
constexpr QuantLib::BigNatural kDefaultPipelineSanityBuehlerMcSeed = 701;
constexpr QuantLib::BigNatural kDefaultBsSeed = 1701;
constexpr QuantLib::Size kDefaultPipelineSanitySubbanks = 1;
constexpr QuantLib::BigNatural kSubbankSeedStride = 1000003ULL;

extern QuantLib::BigNatural gBsSeed;
extern QuantLib::Size gPipelineSanityMcSamples;

QuantLib::Real qlEquityForward(const MarketData& md, const QuantLib::Date& t);

QuantLib::ext::shared_ptr<QuantLib::BlackScholesMertonProcess> makeQlRepoDividendBsProcess(
    const MarketData& md, const QuantLib::Date& expiry, QuantLib::Real strikeS);

std::string maturityLabel(int months);

QuantLib::Real priceBsEuropean(
    const QuantLib::ext::shared_ptr<QuantLib::BlackScholesMertonProcess>& process,
    const QuantLib::Date& expiry, QuantLib::Real strikeS);

QuantLib::Real priceBsCashDigitalCall(const MarketData& md, const QuantLib::Date& expiry,
                                      QuantLib::Real strikeS);

/** @brief Flat BS cash digital put: pays 1 in S if @f$S_T < K@f$. */
QuantLib::Real priceBsCashDigitalPut(const MarketData& md, const QuantLib::Date& expiry,
                                     QuantLib::Real strikeS);

/**
 * @brief Independent coupon strip @f$\sum_i c\,P(DF_i\,\mathbf{1}_{S_i<K})@f$ (no knock-out linkage).
 */
QuantLib::Real priceBsIndependentCouponDigitalStrip(const MarketData& md,
                                                    const std::vector<QuantLib::Date>& obsDates,
                                                    QuantLib::Real couponAmount,
                                                    QuantLib::Real barrierS);

/** @brief Autocall knock-out on a flat BS MC bank (same payoff as @c AutocallMcBuehlerOption). */
BuehlerMcPathPricingResult priceAutocallKnockOutFromBsFlatSavePath(
    const BsFlatMcSavePath& savePath,
    const MarketData& md,
    const std::vector<QuantLib::Date>& obsDates,
    const QuantLib::Real couponAmount,
    const QuantLib::Real barrierS,
    const QuantLib::Real notional,
    AutocallCouponStyle couponStyle = AutocallCouponStyle::Phoenix);

BuehlerMcPathPricingResult priceBsAutocallFromSavePath(const BsFlatMcSavePath& savePath,
                                                       const OptionContractParams& params,
                                                       const AutocallMcTerms& terms,
                                                       const MarketData& md);

QuantLib::Real priceBsStockDigital(
    const QuantLib::ext::shared_ptr<QuantLib::BlackScholesMertonProcess>& process,
    const QuantLib::Date& expiry, QuantLib::Real strikeS);

QuantLib::Real priceBsGeometricAsianCallFlat(const MarketData& md, const QuantLib::Date& expiry,
                                             QuantLib::Real strikeS,
                                             const std::vector<QuantLib::Date>& fixingDates);

QuantLib::Real priceBsArithmeticAsianCallFlat(const MarketData& md, const QuantLib::Date& expiry,
                                              QuantLib::Real strikeS,
                                              const std::vector<QuantLib::Date>& fixingDates);

AsianMcFourPayoffs priceBsAsianAllPayoffsFromSavePath(const BsFlatMcSavePath& savePath,
                                                      const OptionContractParams& params,
                                                      const MarketData& md);

BarrierMcFourPayoffs priceBsBarrierAllPayoffsFromSavePath(const BsFlatMcSavePath& savePath,
                                                          const OptionContractParams& params,
                                                          const MarketData& md);

LookbackMcTwoPayoffs priceBsLookbackAllPayoffsFromSavePath(const BsFlatMcSavePath& savePath,
                                                            const OptionContractParams& params,
                                                            const MarketData& md);

QuantLib::Real priceBsRangeAccrual(const MarketData& md,
                                  const std::vector<QuantLib::Date>& observationDates,
                                  QuantLib::Real strikeLowS, QuantLib::Real strikeUpS);

std::vector<QuantLib::Date> monthlyObservationDates(const MarketData& md,
                                                    const QuantLib::Date& horizon);

void printTenorBanner(const std::string& maturity);
void printFdTableHeader();
void printAsianTableHeader();
void printMcCompareRow(const std::string& product, QuantLib::Real mcValue,
                       QuantLib::Real mcStderr, QuantLib::Real bsRef, double& sumAbsErr);
void printFdRow(const std::string& product, QuantLib::Real npvBs, QuantLib::Real npvBuehler,
                double& sumAbsErr);
void printAsianRow(const std::string& product, const BuehlerMcPathPricingResult& mc,
                   QuantLib::Real bsRef, double& sumAbsErr);
void printBarrierRow(const std::string& product, const BuehlerMcPathPricingResult& mc,
                     QuantLib::Real bsRef, double& sumAbsErr);
void printLookbackRow(const std::string& product, const BuehlerMcPathPricingResult& mc,
                      QuantLib::Real bsRef, double& sumAbsErr);
void printAutocallRow(const std::string& product, const BuehlerMcPathPricingResult& mc,
                      QuantLib::Real bsRef, double& sumAbsErr);
void printSanityLegend(QuantLib::BigNatural buehlerMcSeed, QuantLib::BigNatural bsSeed,
                       QuantLib::Size nSubbanks, QuantLib::Size subbankSamples);

} // namespace bs_flat_reference

#endif
