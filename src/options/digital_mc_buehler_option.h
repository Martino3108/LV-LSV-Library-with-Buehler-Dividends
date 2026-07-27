/**
 * @file digital_mc_buehler_option.h
 * @brief European digitals MC on BuehlerFixingSavePath (LV or LSV bank).
 *
 * Cash: payoff on @c xLevel with @f$k_x@f$ from @f$K_S@f$.
 * Asset in S: payoff @f$S_T\mathbf{1}_{\mathrm{ITM}}@f$ on @c sLevel; asset in X on @c xLevel.
 */

#ifndef DIGITAL_MC_BUEHLER_OPTION_H
#define DIGITAL_MC_BUEHLER_OPTION_H

#include "buehler_mc_path_pricing.h"
#include "buehler_fixing_save_path.h"
#include "option.h"

class BuehlerModel;

/** @brief Cash- or asset-or-nothing digital from @c buehler.fixingSavePath(). */
class DigitalMcBuehlerOption : public Option {
public:
    explicit DigitalMcBuehlerOption(OptionContractParams params,
                                    BuehlerOptionPriceSpace quoteSpace = BuehlerOptionPriceSpace::X,
                                    bool assetOrNothing = false);

    QuantLib::Real price(const BuehlerModel& buehler) const override;
    BuehlerMcPathPricingResult priceWithStdError(const BuehlerModel& buehler) const;
    BuehlerOptionPriceSpace quotedPriceSpace() const override { return quoteSpace_; }
    std::string scenarioExportBaseName() const override;

    static BuehlerMcPathPricingResult priceFromSavePath(const BuehlerFixingSavePath& savePath,
                                                        const OptionContractParams& params,
                                                        const BuehlerModel& buehler,
                                                        BuehlerOptionPriceSpace quoteSpace,
                                                        bool assetOrNothing);

    bool assetOrNothing() const { return assetOrNothing_; }

private:
    BuehlerOptionPriceSpace quoteSpace_;
    bool assetOrNothing_;
};

#endif
