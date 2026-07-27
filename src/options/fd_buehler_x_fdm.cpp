/**
 * @file fd_buehler_x_fdm.cpp
 * @brief Shared Buehler pure-X FD grid defaults and FdmBlackScholesSolver pricing.
 */

#include "fd_buehler_x_fdm.h"
#include "buehler_model.h"
#include <ql/exercise.hpp>
#include <ql/methods/finitedifferences/meshers/fdmblackscholesmesher.hpp>
#include <ql/methods/finitedifferences/meshers/fdmmeshercomposite.hpp>
#include <ql/methods/finitedifferences/solvers/fdmblackscholessolver.hpp>
#include <ql/instruments/payoffs.hpp>
#include <ql/methods/finitedifferences/stepconditions/fdmstepconditioncomposite.hpp>
#include <ql/methods/finitedifferences/utilities/fdminnervaluecalculator.hpp>
#include <ql/processes/blackscholesprocess.hpp>
#include <list>
#include <vector>

namespace {

using namespace QuantLib;

std::vector<Time> buildFrontLoadedRollbackTimes(const Time maturity, const Size nSteps) {
    std::vector<Time> times;
    if (nSteps <= 1 || maturity <= 0.0)
        return times;
    times.reserve(nSteps - 1);
    for (Size k = 1; k < nSteps; ++k) {
        const Real w = static_cast<Real>(k) / static_cast<Real>(nSteps);
        times.push_back(maturity * w * w);
    }
    return times;
}

} // namespace

QuantLib::Size effectiveBuehlerFdTimeSteps(const BuehlerModel& buehler,
                                           const QuantLib::Date& expiry,
                                           QuantLib::Size tGridPerYear) {
    using namespace QuantLib;
    const Real T = buehler.dayCounter().yearFraction(buehler.today(), expiry);
    QL_REQUIRE(T > 0.0, "effectiveBuehlerFdTimeSteps: non-positive time to expiry");
    QL_REQUIRE(tGridPerYear > 0, "effectiveBuehlerFdTimeSteps: tGridPerYear must be positive");
    return std::max<Size>(1, static_cast<Size>(std::ceil(tGridPerYear * T)));
}

QuantLib::ext::shared_ptr<QuantLib::GeneralizedBlackScholesProcess>
makeBuehlerPureXLocalVolProcess(const BuehlerModel& buehler) {
    using namespace QuantLib;
    auto xCurve = ext::make_shared<FlatForward>(buehler.today(), 0.0, buehler.dayCounter());
    xCurve->enableExtrapolation();
    const Handle<YieldTermStructure> xTs(xCurve);
    auto xSpotQuote = ext::make_shared<SimpleQuote>(1.0);
    const Handle<Quote> xSpot(xSpotQuote);
    const Handle<LocalVolTermStructure>& fixedLvX = buehler.fixedPureLocalVolTs();
    return ext::make_shared<GeneralizedBlackScholesProcess>(
        xSpot, xTs, xTs, buehler.pureBlackVolTs(), fixedLvX);
}

QuantLib::Real fdVanillaNPVInX(
    const QuantLib::ext::shared_ptr<QuantLib::GeneralizedBlackScholesProcess>& process,
    const QuantLib::ext::shared_ptr<QuantLib::StrikedTypePayoff>& payoff,
    const QuantLib::ext::shared_ptr<QuantLib::Exercise>& exercise,
    QuantLib::Size xGrid,
    QuantLib::Size timeSteps,
    QuantLib::Real mesherStrikeX,
    const Real xMinConstraint,
    const Real xMaxConstraint) {
    using namespace QuantLib;

    const Time maturity = process->time(exercise->lastDate());
    const FdmSchemeDesc schemeDesc = FdmSchemeDesc::CrankNicolson();

    const ext::shared_ptr<Fdm1dMesher> equityMesher = ext::make_shared<FdmBlackScholesMesher>(
        xGrid, process, maturity, mesherStrikeX, xMinConstraint, xMaxConstraint, 0.0001, 1.5,
        std::pair<Real, Real>(Null<Real>(), Null<Real>()), DividendSchedule(), nullptr, 0.0);
    const ext::shared_ptr<FdmMesher> mesher = ext::make_shared<FdmMesherComposite>(equityMesher);
    const ext::shared_ptr<FdmInnerValueCalculator> calculator =
        ext::make_shared<FdmLogInnerValue>(payoff, mesher, 0);

    const ext::shared_ptr<FdmStepConditionComposite> vanillaConditions =
        FdmStepConditionComposite::vanillaComposite(
            DividendSchedule(), exercise, mesher, calculator,
            process->riskFreeRate()->referenceDate(), process->riskFreeRate()->dayCounter());

    std::list<std::vector<Time>> stoppingTimeLists;
    stoppingTimeLists.push_back(buildFrontLoadedRollbackTimes(maturity, timeSteps));
    if (!vanillaConditions->stoppingTimes().empty())
        stoppingTimeLists.push_back(vanillaConditions->stoppingTimes());
    const ext::shared_ptr<FdmStepConditionComposite> conditions =
        ext::make_shared<FdmStepConditionComposite>(stoppingTimeLists,
                                                  vanillaConditions->conditions());

    const FdmBoundaryConditionSet boundaries;
    const FdmSolverDesc solverDesc = {mesher,     boundaries, conditions, calculator,
                                      maturity,   timeSteps,  0};

    const ext::shared_ptr<FdmBlackScholesSolver> solver =
        ext::make_shared<FdmBlackScholesSolver>(Handle<GeneralizedBlackScholesProcess>(process),
                                                mesherStrikeX, solverDesc, schemeDesc, true);

    return solver->valueAt(process->x0());
}
