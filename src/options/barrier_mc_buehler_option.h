/**
 * @file barrier_mc_buehler_option.h
 * @brief Discrete barrier options on the Buehler MC save path (quoted in S only).
 *
 * Payoffs use @c sLevel on the save path; returns @f$C_S = P(T)\,\mathbb{E}[\mathrm{payoff}_S]@f$.
 * Monitoring on @c params.observationDates (e.g. monthly); if empty, falls back per
 * @c params.observationFrequency: monthly @c Following schedule through @c params.expiry
 * (expiry appended when missing), or every save-path fixing when daily.
 * Payoffs use @c sLevel; knock checked at each observation on the same barrier in S.
 * Returns @f$P(0,T)\,\mathbb{E}[\mathrm{payoff}_S]@f$ with zero rebate.
 */

#ifndef BARRIER_MC_BUEHLER_OPTION_H
#define BARRIER_MC_BUEHLER_OPTION_H

#include "buehler_fixing_save_path.h"
#include "buehler_mc_path_pricing.h"
#include "option.h"
#include <string>

class BuehlerModel;

/** @brief Knock direction for discrete barrier monitoring in S. */
enum class BarrierMcType { DownOut, DownIn, UpOut, UpIn };

struct BarrierMcFourPayoffs {
    BuehlerMcPathPricingResult downOut;
    BuehlerMcPathPricingResult downIn;
    BuehlerMcPathPricingResult upOut;
    BuehlerMcPathPricingResult upIn;
};

/**
 * @brief Discrete barrier from @c buehler.fixingSavePath() after @c simulateFixingPaths.
 * @note Set @c params.barrierDown for down barriers, @c params.barrierUp for up barriers.
 */
class BarrierMcBuehlerOption : public Option {
public:
    explicit BarrierMcBuehlerOption(OptionContractParams params,
                                    BarrierMcType barrierType = BarrierMcType::DownOut);

    QuantLib::Real price(const BuehlerModel& buehler) const override;
    BuehlerMcPathPricingResult priceWithStdError(const BuehlerModel& buehler) const;
    BarrierMcFourPayoffs priceAllPayoffs(const BuehlerModel& buehler) const;
    BuehlerOptionPriceSpace quotedPriceSpace() const override { return BuehlerOptionPriceSpace::S; }
    std::string scenarioExportBaseName() const override;

    static BuehlerMcPathPricingResult priceFromSavePath(const BuehlerFixingSavePath& savePath,
                                                        const OptionContractParams& params,
                                                        const BuehlerModel& buehler,
                                                        BarrierMcType barrierType);

    static BarrierMcFourPayoffs priceAllPayoffsFromSavePath(const BuehlerFixingSavePath& savePath,
                                                            const OptionContractParams& params,
                                                            const BuehlerModel& buehler);

    BarrierMcType barrierType() const { return barrierType_; }

private:
    BarrierMcType barrierType_;
};

#endif
