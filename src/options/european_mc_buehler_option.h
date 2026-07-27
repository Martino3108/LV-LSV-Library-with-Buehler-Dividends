/**
 * @file european_mc_buehler_option.h
 * @brief European vanilla MC on BuehlerFixingSavePath (LV or LSV bank).
 *
 * Prices on @c xLevel at expiry; call/put payoffs evaluated directly on paths
 * (no put–call parity; FD vanillas still use parity for puts).
 */

#ifndef EUROPEAN_MC_BUEHLER_OPTION_H
#define EUROPEAN_MC_BUEHLER_OPTION_H

#include "buehler_mc_path_pricing.h"
#include "buehler_fixing_save_path.h"
#include "option.h"

class BuehlerModel;

/** @brief European call/put from @c buehler.fixingSavePath() after @c simulateFixingPaths. */
class EuropeanMcBuehlerOption : public Option {
public:
    explicit EuropeanMcBuehlerOption(OptionContractParams params,
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
