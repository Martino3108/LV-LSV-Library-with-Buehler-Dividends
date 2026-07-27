/**
 * @file mc_observation_schedule.cpp
 */

#include "mc_observation_schedule.h"
#include "buehler_model.h"
#include <algorithm>
#include <vector>

namespace {

using namespace QuantLib;

std::vector<Date> sortedUniqueDates(std::vector<Date> dates) {
    std::sort(dates.begin(), dates.end());
    dates.erase(std::unique(dates.begin(), dates.end()), dates.end());
    return dates;
}

} // namespace

std::vector<Date> mcObservationDatesMonthlyThroughExpiry(const BuehlerModel& buehler,
                                                         const Date& expiry) {
    QL_REQUIRE(expiry > buehler.today(),
               "mcObservationDatesMonthlyThroughExpiry: expiry must be after today");

    std::vector<Date> dates;
    Date d = buehler.today();
    while (true) {
        d = buehler.calendar().advance(d, 1, Months, Following);
        if (d > expiry)
            break;
        dates.push_back(d);
    }
    if (dates.empty() || dates.back() != expiry)
        dates.push_back(expiry);
    QL_REQUIRE(!dates.empty(), "mcObservationDatesMonthlyThroughExpiry: empty schedule");
    return dates;
}

std::vector<Date> resolveMcObservationDates(const BuehlerModel& buehler,
                                            const OptionContractParams& params,
                                            const char* productLabel) {
    if (!params.observationDates.empty())
        return sortedUniqueDates(params.observationDates);

    QL_REQUIRE(params.expiry > buehler.today(),
               productLabel << ": expiry must be after today when observationDates is empty");

    if (params.observationFrequency == McObservationFrequency::Daily) {
        QL_REQUIRE(buehler.hasFixingSavePath(),
                   productLabel << ": daily observation frequency requires a simulated save path");
        return bankFixingsThroughExpiry(buehler.fixingSavePath().fixingDates(), params.expiry);
    }
    return mcObservationDatesMonthlyThroughExpiry(buehler, params.expiry);
}

std::vector<Date> bankFixingsThroughExpiry(const std::vector<Date>& bankFixings,
                                           const Date& expiry) {
    std::vector<Date> out;
    for (const Date& d : bankFixings) {
        if (d > expiry)
            break;
        out.push_back(d);
    }
    QL_REQUIRE(!out.empty(), "bankFixingsThroughExpiry: no fixings on save path before expiry");
    return out;
}

std::vector<Date> bankFixingsLastDatePerMonth(const std::vector<Date>& bankFixings,
                                              const Date& expiry) {
    std::vector<Date> out;
    for (const Date& d : bankFixings) {
        if (d > expiry)
            break;
        if (out.empty()) {
            out.push_back(d);
            continue;
        }
        if (d.month() == out.back().month() && d.year() == out.back().year())
            out.back() = d;
        else
            out.push_back(d);
    }
    QL_REQUIRE(!out.empty(),
               "bankFixingsLastDatePerMonth: no bank fixings on or before expiry");
    return out;
}

std::vector<Date> bankFixingsLastDatePerYear(const std::vector<Date>& bankFixings,
                                             const Date& expiry) {
    std::vector<Date> out;
    for (const Date& d : bankFixings) {
        if (d > expiry)
            break;
        if (out.empty()) {
            out.push_back(d);
            continue;
        }
        if (d.year() == out.back().year())
            out.back() = d;
        else
            out.push_back(d);
    }
    QL_REQUIRE(!out.empty(), "bankFixingsLastDatePerYear: no bank fixings on or before expiry");
    return out;
}
