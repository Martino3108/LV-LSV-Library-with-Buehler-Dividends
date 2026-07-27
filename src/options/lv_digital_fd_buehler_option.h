/**
 * @file lv_digital_fd_buehler_option.h
 * @brief European digitals via pure-X FD.
 */

#ifndef LV_DIGITAL_FD_BUEHLER_OPTION_H
#define LV_DIGITAL_FD_BUEHLER_OPTION_H

#include "fd_buehler_x_fdm.h"
#include "option.h"

/**
 * @brief Cash- or asset-or-nothing digital on fixed pure-X LV.
 * FD on call in X only; put via parity (cash: @f$1-c_{\mathrm{cash}}@f$,
 * asset: @f$\mathbb{E}[X_T]-c_{\mathrm{asset}}@f$ with @f$\mathbb{E}[X_T]=1@f$ on unit X).
 * Asset in S: @f$P(T)\,[A(T)\,c_X^{\mathrm{asset}}+D(T)\,c_X^{\mathrm{cash}}]@f$.
 */
class LvDigitalFdBuehlerOption : public Option {
public:
    explicit LvDigitalFdBuehlerOption(OptionContractParams params,
                                    QuantLib::Size tGridPerYear = kDefaultFdTGridPerYear,
                                    QuantLib::Size xGrid = kDefaultFdXGrid,
                                    bool assetOrNothing = false);

    explicit LvDigitalFdBuehlerOption(OptionContractParams params,
                                    BuehlerOptionPriceSpace buehlerPriceSpace,
                                    QuantLib::Size tGridPerYear = kDefaultFdTGridPerYear,
                                    QuantLib::Size xGrid = kDefaultFdXGrid,
                                    bool assetOrNothing = false);

    QuantLib::Real price(const BuehlerModel& buehler) const override;
    std::string scenarioExportBaseName() const override;
    BuehlerOptionPriceSpace quotedPriceSpace() const override { return quoteSpace_; }

    static QuantLib::Real pureXStrikeFromSpot(const BuehlerModel& b,
                                              const QuantLib::Date& expiry,
                                              QuantLib::Real strikeS);

    QuantLib::Size fdTGridPerYear() const { return tGridPerYear_; }
    QuantLib::Size fdSpaceGridSteps() const { return xGrid_; }
    QuantLib::Size effectiveFdTimeSteps(const BuehlerModel& buehler) const;

private:
    QuantLib::Real priceCashDigitalCallInX(const BuehlerModel& buehler,
                                           QuantLib::Real kx) const;
    QuantLib::Real priceAssetDigitalCallInX(const BuehlerModel& buehler,
                                            QuantLib::Real kx) const;

    QuantLib::Size tGridPerYear_;
    QuantLib::Size xGrid_;
    bool assetOrNothing_;
    BuehlerOptionPriceSpace quoteSpace_ = BuehlerOptionPriceSpace::X;
};

#endif
