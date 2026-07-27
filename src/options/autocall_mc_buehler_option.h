/**
 * @file autocall_mc_buehler_option.h
 * @brief Phoenix / Athena autocall on the Buehler MC save path (quoted in S).
 *
 * Reference notional defaults to spot @f$S_0@f$ (affine map @f$x=1@f$). Barrier and coupon
 * are fractions of that reference. While alive, each observation accrues
 * @f$c = \mathrm{couponRate}\times\mathrm{notional}@f$. If @f$S \ge B@f$ (early autocall)
 * or at the final observation the product redeems:
 * - @b phoenix: @f$c@f$ paid at every observation date; notional at redemption.
 * - @b athena: accrued coupons plus notional paid in one lump sum at redemption.
 * Monitoring on @c params.observationDates; if empty, falls back per
 * @c params.observationFrequency (monthly @c Following schedule through @c params.expiry,
 * or every save-path fixing when daily).
 */

#ifndef AUTOCALL_MC_BUEHLER_OPTION_H
#define AUTOCALL_MC_BUEHLER_OPTION_H

#include "buehler_fixing_save_path.h"
#include "buehler_mc_path_pricing.h"
#include "option.h"
#include <string>

class BuehlerModel;

/** @brief Coupon payment style for @ref AutocallMcBuehlerOption. */
enum class AutocallCouponStyle {
    Phoenix,
    Athena
};

/** @brief Parse @c couponStyle (@c "phoenix" or @c "athena", case-insensitive). */
AutocallCouponStyle parseAutocallCouponStyle(const std::string& couponStyle);

/** @brief Export label for @c couponStyle. */
const char* autocallCouponStyleLabel(AutocallCouponStyle style);

/** @brief Economic terms for @ref AutocallMcBuehlerOption (all amounts in S). */
struct AutocallMcTerms {
    /** @brief Coupon per observation as a fraction of reference notional (e.g. 0.03 per period). */
    QuantLib::Real couponRatePerPeriod = QuantLib::Null<QuantLib::Real>();
    /** @brief Autocall barrier as a fraction of reference notional (e.g. 1.0 = 100% of spot). */
    QuantLib::Real barrierFractionOfSpot = QuantLib::Null<QuantLib::Real>();
    /**
     * @brief Reference notional for coupon sizing (typically @f$S_0@f$).
     * @c Null means @c buehler.mapXtoS(today, 1).
     */
    QuantLib::Real referenceNotional = QuantLib::Null<QuantLib::Real>();
    /** @brief @c "phoenix" (coupon each obs) or @c "athena" (accrued coupons at redemption). */
    std::string couponStyle = "phoenix";
};

class AutocallMcBuehlerOption : public Option {
public:
    explicit AutocallMcBuehlerOption(OptionContractParams params, AutocallMcTerms terms);

    QuantLib::Real price(const BuehlerModel& buehler) const override;
    BuehlerMcPathPricingResult priceWithStdError(const BuehlerModel& buehler) const;
    BuehlerOptionPriceSpace quotedPriceSpace() const override { return BuehlerOptionPriceSpace::S; }
    std::string scenarioExportBaseName() const override;

    static BuehlerMcPathPricingResult priceFromSavePath(const BuehlerFixingSavePath& savePath,
                                                        const OptionContractParams& params,
                                                        const AutocallMcTerms& terms,
                                                        const BuehlerModel& buehler);

    const AutocallMcTerms& autocallTerms() const { return terms_; }

private:
    AutocallMcTerms terms_;
};

#endif
