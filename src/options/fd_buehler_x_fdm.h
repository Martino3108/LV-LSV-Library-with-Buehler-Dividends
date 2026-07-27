/**
 * @file fd_buehler_x_fdm.h
 * @brief Shared pure-X FD defaults and QuantLib FDM helpers.
 */

#ifndef FD_BUEHLER_X_FDM_H
#define FD_BUEHLER_X_FDM_H

#include <ql/quantlib.hpp>

class BuehlerModel;

constexpr QuantLib::Size kDefaultFdTGridPerYear = 100;
constexpr QuantLib::Size kDefaultFdXGrid = 200;
/** Digital (discontinuous-payoff) FD refinement, applied to BOTH the cash and the
 *  asset legs; vanillas keep @c kDefaultFdXGrid / base steps. The density read at
 *  the strike converges slowly for step payoffs and the mesh spacing at the strike
 *  grows with sqrt(T): on the base grid a 2y cash digital carries ~6e-3 label error
 *  (grid-doubling probe, runs/digital_cash_val/fd_convergence_probe.csv); the
 *  refined grid brings the residual under ~1e-3. */
constexpr QuantLib::Size kFdDigitalXGridMultiplier = 4;
constexpr QuantLib::Size kFdDigitalTimeStepMultiplier = 2;
/** Time mesh: t_k = T * (k/N)^2 (fixed; not configurable). */

/** @brief @c max(1, ceil(tGridPerYear * T)). */
QuantLib::Size effectiveBuehlerFdTimeSteps(const BuehlerModel& buehler,
                                           const QuantLib::Date& expiry,
                                           QuantLib::Size tGridPerYear);

/** @brief Unit forward X process with @c fixedPureLocalVolTs. */
QuantLib::ext::shared_ptr<QuantLib::GeneralizedBlackScholesProcess>
makeBuehlerPureXLocalVolProcess(const BuehlerModel& buehler);

/** @brief European FD NPV in X (Crank–Nicolson, w^2 rollback times). */
QuantLib::Real fdVanillaNPVInX(
    const QuantLib::ext::shared_ptr<QuantLib::GeneralizedBlackScholesProcess>& process,
    const QuantLib::ext::shared_ptr<QuantLib::StrikedTypePayoff>& payoff,
    const QuantLib::ext::shared_ptr<QuantLib::Exercise>& exercise,
    QuantLib::Size xGrid,
    QuantLib::Size timeSteps,
    QuantLib::Real mesherStrikeX,
    QuantLib::Real xMinConstraint = QuantLib::Null<QuantLib::Real>(),
    QuantLib::Real xMaxConstraint = QuantLib::Null<QuantLib::Real>());

#endif
