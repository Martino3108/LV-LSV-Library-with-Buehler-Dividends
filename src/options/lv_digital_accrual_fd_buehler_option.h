/**
 * @file lv_digital_accrual_fd_buehler_option.h
 * @brief Range digital accrual (cash digitals at Low and Up per observation).
 */

#ifndef LV_DIGITAL_ACCRUAL_FD_BUEHLER_OPTION_H
#define LV_DIGITAL_ACCRUAL_FD_BUEHLER_OPTION_H

#include "fd_buehler_x_fdm.h"
#include "option.h"

/** @brief Mean over @c observationDates of (cash digital at Low − at Up) in X or S. */
class LvDigitalAccrualFdBuehlerOption : public Option {
public:
    explicit LvDigitalAccrualFdBuehlerOption(OptionContractParams params,
                                           QuantLib::Size tGridPerYear = kDefaultFdTGridPerYear,
                                           QuantLib::Size xGrid = kDefaultFdXGrid);

    explicit LvDigitalAccrualFdBuehlerOption(OptionContractParams params,
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

private:
    /** @param kx pure-X strike (already mapped by the caller per this option's quote space). */
    QuantLib::Real priceCashDigitalCallInX(const BuehlerModel& buehler,
                                           const QuantLib::Date& expiry,
                                           QuantLib::Real kx) const;

    QuantLib::Size tGridPerYear_;
    QuantLib::Size xGrid_;
    BuehlerOptionPriceSpace quoteSpace_ = BuehlerOptionPriceSpace::X;
};

#endif
