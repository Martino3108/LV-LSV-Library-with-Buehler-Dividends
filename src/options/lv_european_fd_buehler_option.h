/**
 * @file lv_european_fd_buehler_option.h
 * @brief European vanilla via pure-X FD.
 */

#ifndef LV_EUROPEAN_FD_BUEHLER_OPTION_H
#define LV_EUROPEAN_FD_BUEHLER_OPTION_H

#include "fd_buehler_x_fdm.h"
#include "option.h"

/**
 * @brief European call/put on fixed pure-X LV.
 * Put via parity in X; S mapping @c P(0,T)*A(T)*NPV_X with @c A=F(0,T)-D(T).
 */
class LvEuropeanFdBuehlerOption : public Option {
public:
    explicit LvEuropeanFdBuehlerOption(OptionContractParams params,
                                     QuantLib::Size tGridPerYear = kDefaultFdTGridPerYear,
                                     QuantLib::Size xGrid = kDefaultFdXGrid);

    explicit LvEuropeanFdBuehlerOption(OptionContractParams params,
                                     BuehlerOptionPriceSpace buehlerPriceSpace,
                                     QuantLib::Size tGridPerYear = kDefaultFdTGridPerYear,
                                     QuantLib::Size xGrid = kDefaultFdXGrid);

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
    QuantLib::Real pureXStrikeForPricing(const BuehlerModel& buehler) const;
    QuantLib::Real priceCallInX(const BuehlerModel& buehler) const;

    QuantLib::Size tGridPerYear_;
    QuantLib::Size xGrid_;
    BuehlerOptionPriceSpace quoteSpace_ = BuehlerOptionPriceSpace::X;
};

#endif
