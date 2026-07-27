/**
 * @file asian_mc_buehler_option.h
 * @brief Discrete Asian options on the Buehler model MC save path (quoted in S only).
 *
 * Payoffs use @c sLevel on the save path; returns @f$C_S = P(T)\,\mathbb{E}[\mathrm{payoff}_S]@f$.
 * Contract @c params.strike is @f$K_S@f$. Monitoring on @c params.observationDates; if empty,
 * falls back per @c params.observationFrequency (monthly @c Following schedule through
 * @c params.expiry, or every save-path fixing when daily).
 */

#ifndef ASIAN_MC_BUEHLER_OPTION_H
#define ASIAN_MC_BUEHLER_OPTION_H

#include "buehler_fixing_save_path.h"
#include "buehler_mc_path_pricing.h"
#include "option.h"
#include <string>

class BuehlerModel;

enum class AsianMcPayoffKind {
    GeometricFixed,
    GeometricFloating,
    ArithmeticFixed,
    ArithmeticFloating
};

struct AsianMcFourPayoffs {
    BuehlerMcPathPricingResult geometricFixed;
    BuehlerMcPathPricingResult geometricFloating;
    BuehlerMcPathPricingResult arithmeticFixed;
    BuehlerMcPathPricingResult arithmeticFloating;
};

enum class AsianMcAverageType { Arithmetic, Geometric };

enum class AsianMcStrikeStyle { Fixed, Floating };

AsianMcPayoffKind asianMcPayoffKind(AsianMcAverageType averageType, AsianMcStrikeStyle strikeStyle);

/** @brief Prices from @c buehler.fixingSavePath() (call @c simulateFixingPaths first). */
class AsianMcBuehlerOption : public Option {
public:
    explicit AsianMcBuehlerOption(OptionContractParams params,
                                  AsianMcAverageType averageType = AsianMcAverageType::Arithmetic,
                                  AsianMcStrikeStyle strikeStyle = AsianMcStrikeStyle::Fixed);

    QuantLib::Real price(const BuehlerModel& buehler) const override;
    BuehlerMcPathPricingResult priceWithStdError(const BuehlerModel& buehler) const;
    AsianMcFourPayoffs priceAllPayoffs(const BuehlerModel& buehler) const;
    BuehlerOptionPriceSpace quotedPriceSpace() const override { return BuehlerOptionPriceSpace::S; }

    static BuehlerMcPathPricingResult priceFromSavePath(const BuehlerFixingSavePath& savePath,
                                                      AsianMcPayoffKind payoffKind,
                                                      const OptionContractParams& params,
                                                      const BuehlerModel& buehler);

    static AsianMcFourPayoffs priceAllPayoffsFromSavePath(const BuehlerFixingSavePath& savePath,
                                                          const OptionContractParams& params,
                                                          const BuehlerModel& buehler);

    std::string scenarioExportBaseName() const override;

    AsianMcAverageType averageType() const { return averageType_; }
    AsianMcStrikeStyle strikeStyle() const { return strikeStyle_; }

private:
    AsianMcAverageType averageType_;
    AsianMcStrikeStyle strikeStyle_;
};

#endif
