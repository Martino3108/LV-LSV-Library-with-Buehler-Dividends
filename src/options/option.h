/**
 * @file option.h
 * @brief Option contract parameters and polymorphic Buehler pricers.
 */

#ifndef OPTION_H
#define OPTION_H

#include <ql/quantlib.hpp>
#include <optional>
#include <string>
#include <vector>

class BuehlerModel;

/** @brief Quote space for MC/FD pricers (Asian/barrier MC: S only; European/digital/accrual MC: X or S). */
enum class BuehlerOptionPriceSpace {
    X,
    S
};

/** @brief Default monitoring schedule used when @c observationDates is empty. */
enum class McObservationFrequency {
    Monthly, ///< monthly Following grid through expiry (historical default)
    Daily    ///< every date stored on the MC save path up to expiry
};

/** @brief Economic contract terms (strikes and schedules are in S unless noted). */
struct OptionContractParams {
    QuantLib::Real strike = QuantLib::Null<QuantLib::Real>();
    QuantLib::Date expiry;
    bool isCall = true;
    std::optional<QuantLib::Real> barrierUp;
    std::optional<QuantLib::Real> barrierDown;
    std::vector<QuantLib::Date> observationDates;
    McObservationFrequency observationFrequency = McObservationFrequency::Monthly;
    QuantLib::Real strikeLow = QuantLib::Null<QuantLib::Real>();
    QuantLib::Real strikeUp = QuantLib::Null<QuantLib::Real>();
};

/** @brief Abstract product: @c price(BuehlerModel) and @c scenarioExportBaseName(). */
class Option {
public:
    explicit Option(OptionContractParams params) : params_(std::move(params)) {}

    virtual ~Option() = default;

    Option(const Option&) = default;
    Option& operator=(const Option&) = default;

    virtual QuantLib::Real price(const BuehlerModel& buehler) const = 0;
    virtual std::string scenarioExportBaseName() const { return "option"; }
    /** @brief Space of @c price(); S-only for Asian/barrier MC; X or S for European/digital/accrual MC and FD. */
    virtual BuehlerOptionPriceSpace quotedPriceSpace() const = 0;

    const OptionContractParams& contractParams() const { return params_; }

protected:
    OptionContractParams params_;
};

#endif
