/**
 * @file market_data_inspect.cpp
 */

#include "market_data_inspect.h"
#include "market_data.h"
#include <iomanip>
#include <iostream>
#include <ql/quantlib.hpp>

void showImportedData(const MarketData& md) {
    using namespace QuantLib;
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "\n=== Imported Data (loaded in MarketData) ===\n";
    std::cout << "Today: " << md.today() << "\n";
    std::cout << "Spot: " << md.spotValue() << "\n";
    std::cout << "Expiries: " << md.expiries().size()
              << " | Strikes: " << md.strikes().size() << "\n";

    std::cout << "\nRisk-free curve nodes:\n";
    for (Size i = 0; i < md.riskFreeDates().size() && i < md.riskFreeZeroRates().size(); ++i) {
        std::cout << "  " << md.riskFreeDates()[i]
                  << " -> " << md.riskFreeZeroRates()[i] << "\n";
    }

    std::cout << "\nRepo curve nodes:\n";
    for (Size i = 0; i < md.repoDates().size() && i < md.repoZeroRates().size(); ++i) {
        std::cout << "  " << md.repoDates()[i]
                  << " -> " << md.repoZeroRates()[i] << "\n";
    }

    std::cout << "\nDividends loaded: " << md.dividendDates().size() << "\n";
    for (Size i = 0; i < md.dividendDates().size() && i < md.dividendAmounts().size(); ++i) {
        std::cout << "  " << md.dividendDates()[i]
                  << " -> " << md.dividendAmounts()[i] << "\n";
    }

    std::cout << "\nImplied vol matrix [strike x expiry] loaded in md:\n";
    std::cout << std::left << std::setw(10) << "K\\T";
    for (const auto& e : md.expiries()) {
        const Time t = md.dayCounter().yearFraction(md.today(), e);
        std::cout << std::setw(10) << t;
    }
    std::cout << "\n";
    std::cout << std::string(10 + 10 * md.expiries().size(), '-') << "\n";
    for (Size i = 0; i < md.strikes().size(); ++i) {
        std::cout << std::setw(10) << md.strikes()[i];
        for (Size j = 0; j < md.expiries().size(); ++j) {
            std::cout << std::setw(10) << md.impliedVols()[i][j];
        }
        std::cout << "\n";
    }

    std::cout << "=============================================\n";
}
