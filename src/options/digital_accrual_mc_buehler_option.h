/**
 * @file digital_accrual_mc_buehler_option.h
 * @brief Range digital accrual MC on BuehlerFixingSavePath (LV or LSV bank).
 *
 * Cash digital call spread per observation date; payoff on paths (no parity).
 */

#ifndef DIGITAL_ACCRUAL_MC_BUEHLER_OPTION_H
#define DIGITAL_ACCRUAL_MC_BUEHLER_OPTION_H

#include "buehler_mc_path_pricing.h"
#include "buehler_fixing_save_path.h"
#include "option.h"

class BuehlerModel;

/** @brief Mean over observation dates of (cash digital at Low − at Up), MC on path bank. */
class DigitalAccrualMcBuehlerOption : public Option {
public:
    explicit DigitalAccrualMcBuehlerOption(OptionContractParams params,
                                           BuehlerOptionPriceSpace quoteSpace = BuehlerOptionPriceSpace::X);

    QuantLib::Real price(const BuehlerModel& buehler) const override;
    BuehlerMcPathPricingResult priceWithStdError(const BuehlerModel& buehler) const;
    BuehlerOptionPriceSpace quotedPriceSpace() const override { return quoteSpace_; }
    std::string scenarioExportBaseName() const override;

    static BuehlerMcPathPricingResult priceFromSavePath(const BuehlerFixingSavePath& savePath,
                                                        const OptionContractParams& params,
                                                        const BuehlerModel& buehler,
                                                        BuehlerOptionPriceSpace quoteSpace);

private:
    BuehlerOptionPriceSpace quoteSpace_;
};

#endif
