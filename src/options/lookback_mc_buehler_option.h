/**
 * @file lookback_mc_buehler_option.h
 * @brief Discrete lookback options on the Buehler MC save path (quoted in S only).
 *
 * Running maximum and minimum of @c sLevel on the observation schedule, market-standard
 * payoffs. Set @c params.observationDates explicitly, or leave empty for the
 * @c params.observationFrequency fallback (monthly @c Following grid through
 * @c params.expiry, or every save-path fixing when daily).
 * Fixed strike: call @f$\max(0, \max S - K_S)@f$ on the maximum, put
 * @f$\max(0, K_S - \min S)@f$ on the minimum.
 * Floating strike: call @f$\max(0, S_T - \min S)@f$ (buy at the low), put
 * @f$\max(0, \max S - S_T)@f$ (sell at the high), with @f$S_T@f$ at @c params.expiry.
 * Returns @f$P(0,T)\,\mathbb{E}[\mathrm{payoff}_S]@f$.
 */

#ifndef LOOKBACK_MC_BUEHLER_OPTION_H
#define LOOKBACK_MC_BUEHLER_OPTION_H

#include "buehler_fixing_save_path.h"
#include "buehler_mc_path_pricing.h"
#include "option.h"
#include <string>

class BuehlerModel;

enum class LookbackMcStrikeStyle { Fixed, Floating };

struct LookbackMcTwoPayoffs {
    BuehlerMcPathPricingResult fixed;
    BuehlerMcPathPricingResult floating;
};

/** @brief Lookback from @c buehler.fixingSavePath() after @c simulateFixingPaths. */
class LookbackMcBuehlerOption : public Option {
public:
    explicit LookbackMcBuehlerOption(OptionContractParams params,
                                     LookbackMcStrikeStyle strikeStyle = LookbackMcStrikeStyle::Fixed);

    QuantLib::Real price(const BuehlerModel& buehler) const override;
    BuehlerMcPathPricingResult priceWithStdError(const BuehlerModel& buehler) const;
    LookbackMcTwoPayoffs priceAllPayoffs(const BuehlerModel& buehler) const;
    BuehlerOptionPriceSpace quotedPriceSpace() const override { return BuehlerOptionPriceSpace::S; }
    std::string scenarioExportBaseName() const override;

    static BuehlerMcPathPricingResult priceFromSavePath(const BuehlerFixingSavePath& savePath,
                                                        const OptionContractParams& params,
                                                        const BuehlerModel& buehler,
                                                        LookbackMcStrikeStyle strikeStyle);

    static LookbackMcTwoPayoffs priceAllPayoffsFromSavePath(const BuehlerFixingSavePath& savePath,
                                                            const OptionContractParams& params,
                                                            const BuehlerModel& buehler);

    LookbackMcStrikeStyle strikeStyle() const { return strikeStyle_; }

private:
    LookbackMcStrikeStyle strikeStyle_;
};

#endif
