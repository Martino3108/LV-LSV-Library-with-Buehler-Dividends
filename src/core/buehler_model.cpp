/**
 * @file buehler_model.cpp
 * @brief BuehlerModel implementation and file-local helpers.
 */

#include "buehler_model.h"
#include "buehler_fixing_path_simulate.h"
#include "buehler_iv_x_arbitrage.h"
#include "buehler_mc_sigma_lookup.h"
#include "fd_buehler_x_fdm.h"
#include "lv_european_fd_buehler_option.h"
#include "market_data.h"
#include "option.h"
#include "ql/math/interpolations/cubicinterpolation.hpp"
#include <ql/pricingengines/blackformula.hpp>
#include <ql/termstructures/volatility/equityfx/fixedlocalvolsurface.hpp>
#include <ql/termstructures/volatility/equityfx/localvoltermstructure.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <vector>

namespace {

using namespace QuantLib;

/** @brief Risk-free and repo discount factors between @p from and @p to (carry building block). */
Real carryGrowthFactor(const Handle<YieldTermStructure>& riskFreeTs,
                       const Handle<YieldTermStructure>& repoTs,
                       const Date& from,
                       const Date& to) {
    if (to < from)
        QL_FAIL("carryGrowthFactor requires to >= from");
    if (to == from)
        return 1.0;
    const DiscountFactor drFrom = riskFreeTs->discount(from, true);
    const DiscountFactor drTo   = riskFreeTs->discount(to,   true);
    const DiscountFactor dqFrom = repoTs->discount(from, true);
    const DiscountFactor dqTo   = repoTs->discount(to,   true);
    QL_REQUIRE(drTo > 0.0 && dqFrom > 0.0,
               "Invalid discount factors while computing carry growth factor");
    return (drFrom / drTo) * (dqTo / dqFrom);
}

bool isPositiveFiniteVol(Volatility sigma) {
    return std::isfinite(sigma) && sigma > 0.0;
}

Volatility safeBlackVol(const Handle<BlackVolTermStructure>& ts, const Date& d, Real kx) {
    if (ts.empty())
        return Null<Volatility>();
    try {
        const Volatility v = ts->blackVol(d, kx, true);
        return isPositiveFiniteVol(v) ? v : Null<Volatility>();
    } catch (...) {
        return Null<Volatility>();
    }
}

Size nearestMarketExpiryIndex(const std::vector<Date>& marketExpiries, const Date& d) {
    Size best = 0;
    auto dist = [&](const Date& e) {
        return std::llabs(static_cast<long long>(e.serialNumber()) -
                         static_cast<long long>(d.serialNumber()));
    };
    long bestDist = dist(marketExpiries[0]);
    for (Size k = 1; k < marketExpiries.size(); ++k) {
        const long dK = dist(marketExpiries[k]);
        if (dK < bestDist) {
            bestDist = dK;
            best = k;
        }
    }
    return best;
}

/** Linear in kx on pre-bicubic implied σ_X (monotonic nodes; avoids bicubic overshoot). */
Volatility impliedVolXFromGrid(const Matrix& impliedVolsX,
                               const std::vector<Real>& marketXStrikes,
                               Size expiryCol,
                               Real kx) {
    QL_REQUIRE(expiryCol < impliedVolsX.columns(), "impliedVolXFromGrid: expiry column out of range");
    QL_REQUIRE(marketXStrikes.size() == impliedVolsX.rows(),
               "impliedVolXFromGrid: strike grid size mismatch");
    if (kx <= marketXStrikes.front())
        return impliedVolsX[0][expiryCol];
    if (kx >= marketXStrikes.back())
        return impliedVolsX[marketXStrikes.size() - 1][expiryCol];
    for (Size i = 0; i + 1 < marketXStrikes.size(); ++i) {
        if (kx <= marketXStrikes[i + 1]) {
            const Real kLo = marketXStrikes[i];
            const Real kHi = marketXStrikes[i + 1];
            const Real w = (kx - kLo) / (kHi - kLo);
            return impliedVolsX[i][expiryCol] * (1.0 - w) + impliedVolsX[i + 1][expiryCol] * w;
        }
    }
    return impliedVolsX[marketXStrikes.size() - 1][expiryCol];
}

/**
 * Wraps the tabulated `FixedLocalVolSurface`: for kx outside [kTabMin, kTabMax] returns σ(t, kTabMin)
 * or σ(t, kTabMax) — flat extension in the strike dimension at each t (same as re-querying on-grid).
 */
class TabulatedKxFlatOutsideLocalVol : public LocalVolTermStructure {
public:
    TabulatedKxFlatOutsideLocalVol(const Date& referenceDate,
                                   const DayCounter& dc,
                                   const Handle<LocalVolTermStructure>& inner,
                                   Real kTabMin,
                                   Real kTabMax)
    : LocalVolTermStructure(referenceDate, NullCalendar(), Following, dc),
      inner_(inner), kTabMin_(kTabMin), kTabMax_(kTabMax) {
        QL_REQUIRE(!inner_.empty(), "inner local vol handle required");
        QL_REQUIRE(kTabMax_ > kTabMin_, "tabulated kx range degenerate");
        registerWith(inner_);
    }
    Date maxDate() const override { return inner_->maxDate(); }
    Real minStrike() const override { return 0.0; }
    Real maxStrike() const override { return QL_MAX_REAL; }

protected:
    Volatility localVolImpl(Time t, Real strike) const override {
        Real k = strike;
        if (k < kTabMin_) k = kTabMin_;
        if (k > kTabMax_) k = kTabMax_;
        return inner_->localVol(t, k, true);
    }

private:
    Handle<LocalVolTermStructure> inner_;
    Real kTabMin_, kTabMax_;
};

/** @brief X-implied vol grid, bicubic wrap, Dupire LV → `FixedLocalVolSurface`. */
Handle<LocalVolTermStructure> buildFixedLocalVolFromPureImpliedX(
    const Date& today,
    const Calendar& calendar,
    const DayCounter& dayCounter,
    const Date& maxDate,
    const Handle<BlackVolTermStructure>& pureBlackVolTs,
    const std::vector<Date>& marketExpiries,
    const std::vector<Real>& marketXStrikes,
    const std::vector<Real>& marketKs,
    const std::vector<Real>& affineAatExp,
    const std::vector<Real>& affineDatExp,
    std::vector<Date>& outDenseExpiries,
    std::vector<Real>& outDenseXStrikes,
    Matrix& outDenseLocalVolX,
    Matrix* preBicubicImpliedVolsXOut = nullptr,
    std::vector<Date>* preBicubicExpiriesOut = nullptr,
    std::vector<Real>* preBicubicXStrikesOut = nullptr,
    BuehlerLvDenseRepairCounts* outRepair = nullptr,
    Handle<BlackVolTermStructure>* impliedVolXBicubicOut = nullptr,
    bool useInjectedMinStrikeKxForSmile = false,
    Real injectedMinStrikeKxForSmile = 0.0) {

    QL_REQUIRE(maxDate > today, "Fixed local-vol build requires maxDate > today");
    QL_REQUIRE(!marketExpiries.empty(), "Fixed local-vol build requires non-empty expiries");
    QL_REQUIRE(marketXStrikes.size() >= 2, "Fixed local-vol build requires at least 2 X strikes");
    QL_REQUIRE(!marketKs.empty(), "Fixed local-vol build requires market strikes");
    QL_REQUIRE(affineAatExp.size() == marketExpiries.size(),
               "affine A size must match expiries");
    QL_REQUIRE(affineDatExp.size() == marketExpiries.size(),
               "affine D size must match expiries");
    QL_REQUIRE(marketXStrikes.front() > 0.0 && std::isfinite(marketXStrikes.front()),
               "Buehler fixed LV: market X strikes must start at a positive finite kx");

    // Implied σ_X: monotonic cubic on kx support; left/right wings linear (full spline slopes). Bicubic in (T,kx).
    // Optional: kx = max_T kx(K_min,T) injected on every smile column before the monotonic cubic.
    Matrix impliedVolsX(marketXStrikes.size(), marketExpiries.size());
    const Size nKmer = marketKs.size();
    for (Size j = 0; j < marketExpiries.size(); ++j) {
        const Real A = affineAatExp[j];
        const Real D = affineDatExp[j];
        auto fillColumnDirect = [&]() {
            for (Size i = 0; i < marketXStrikes.size(); ++i) {
                const Volatility v = pureBlackVolTs->blackVol(
                    marketExpiries[j], marketXStrikes[i], true);
                QL_REQUIRE(std::isfinite(v) && v > 0.0,
                           "Buehler implied σ_X (direct column): invalid Black vol in pure X");
                impliedVolsX[i][j] = v;
            }
        };
        if (A <= 0.0) {
            fillColumnDirect();
            continue;
        }

        std::vector<Real> kxs;
        std::vector<Real> sigs;
        kxs.reserve(nKmer + (useInjectedMinStrikeKxForSmile ? 1 : 0));
        sigs.reserve(nKmer + (useInjectedMinStrikeKxForSmile ? 1 : 0));
        for (Size m = 0; m < nKmer; ++m) {
            const Real kx = (marketKs[m] - D) / A;
            if (!(kx > 0.0) || !std::isfinite(kx))
                continue;
            const Volatility sv = pureBlackVolTs->blackVol(marketExpiries[j], kx, true);
            if (!std::isfinite(sv) || sv <= 0.0)
                continue;
            kxs.push_back(kx);
            sigs.push_back(sv);
        }
        if (useInjectedMinStrikeKxForSmile && injectedMinStrikeKxForSmile > 0.0 &&
            std::isfinite(injectedMinStrikeKxForSmile)) {
            const Volatility svInj =
                pureBlackVolTs->blackVol(marketExpiries[j], injectedMinStrikeKxForSmile, true);
            if (std::isfinite(svInj) && svInj > 0.0) {
                kxs.push_back(injectedMinStrikeKxForSmile);
                sigs.push_back(svInj);
            }
        }
        if (kxs.size() < 2) {
            fillColumnDirect();
            continue;
        }

        std::vector<Size> perm(kxs.size());
        std::iota(perm.begin(), perm.end(), 0);
        std::sort(perm.begin(), perm.end(),
                  [&](Size aIdx, Size bIdx) { return kxs[aIdx] < kxs[bIdx]; });
        std::vector<Real> cx, cy;
        cx.reserve(kxs.size());
        cy.reserve(kxs.size());
        for (Size p : perm) {
            cx.push_back(kxs[p]);
            cy.push_back(sigs[p]);
        }

        std::vector<Real> ux, uy;
        for (Size t = 0; t < cx.size(); ++t) {
            if (ux.empty()) {
                ux.push_back(cx[t]);
                uy.push_back(cy[t]);
                continue;
            }
            const Real tol = 1.0e-9 * std::max(1.0, std::fabs(ux.back()));
            if (std::fabs(cx[t] - ux.back()) <= tol)
                uy.back() = 0.5 * (uy.back() + cy[t]);
            else {
                ux.push_back(cx[t]);
                uy.push_back(cy[t]);
            }
        }
        if (ux.size() < 2) {
            fillColumnDirect();
            continue;
        }

        MonotonicCubicNaturalSpline smileInterp(ux.begin(), ux.end(), uy.begin());
        const Real kxSupportLo = ux.front();
        const Real kxSupportHi = ux.back();
        const Volatility sigLo = smileInterp(kxSupportLo, false);
        const Volatility sigHi = smileInterp(kxSupportHi, false);
        const Real derLo = smileInterp.derivative(kxSupportLo, false);
        const Real derHi = smileInterp.derivative(kxSupportHi, false);
        for (Size i = 0; i < marketXStrikes.size(); ++i) {
            const Real kxTgt = marketXStrikes[i];
            Volatility sigma;
            if (kxTgt < kxSupportLo) {
                QL_REQUIRE(sigLo > 0.0 && std::isfinite(sigLo) && std::isfinite(derLo),
                           "left linear wing requires positive finite sigma and derivative at kx_min");
                sigma = sigLo + (kxTgt - kxSupportLo) * derLo;
            } else if (kxTgt > kxSupportHi) {
                QL_REQUIRE(sigHi > 0.0 && std::isfinite(sigHi) && std::isfinite(derHi),
                           "right linear wing requires positive finite sigma and derivative at kx_max");
                sigma = sigHi + (kxTgt - kxSupportHi) * derHi;
            } else
                sigma = smileInterp(kxTgt, false);
            if (!std::isfinite(sigma) || sigma <= 0.0)
                sigma = uy.front();
            QL_REQUIRE(std::isfinite(sigma) && sigma > 0.0,
                       "Buehler implied σ_X: invalid vol after smile column repair");
            impliedVolsX[i][j] = sigma;
        }
    }

    if (preBicubicImpliedVolsXOut != nullptr) {
        *preBicubicImpliedVolsXOut = impliedVolsX;
        if (preBicubicExpiriesOut != nullptr)
            *preBicubicExpiriesOut = marketExpiries;
        if (preBicubicXStrikesOut != nullptr)
            *preBicubicXStrikesOut = marketXStrikes;
    }

    auto blackSurfaceX = ext::make_shared<BlackVarianceSurface>(
        today, calendar,
        marketExpiries, marketXStrikes,
        impliedVolsX, dayCounter,
        BlackVarianceSurface::ConstantExtrapolation,
        BlackVarianceSurface::ConstantExtrapolation);
    blackSurfaceX->setInterpolation<Bicubic>();
    blackSurfaceX->enableExtrapolation();
    Handle<BlackVolTermStructure> rebuiltPureBlackVolTs(blackSurfaceX);
    if (impliedVolXBicubicOut != nullptr) {
        *impliedVolXBicubicOut = rebuiltPureBlackVolTs;
    }

    // dense time grid: floor at 1M (avoid Dupire deep in short-end extrapolation).
    const Date marketMaxExpiry = *std::max_element(marketExpiries.begin(), marketExpiries.end());
    const Time marketMaxT  = dayCounter.yearFraction(today, marketMaxExpiry);
    const Time timeFloor   = 1.0 / 12.0;
    const Time denseHorizonT = std::max(marketMaxT, timeFloor);
    const Size nDenseExp =
        std::max<Size>(2, static_cast<Size>(std::ceil(denseHorizonT * 12.0)));

    const Integer dayHorizon =
        std::max(1, static_cast<Integer>(std::lround(denseHorizonT * 365.0)));
    const Integer dayFloor =
        std::max(1, static_cast<Integer>(std::lround(timeFloor * 365.0)));

    // Front-loaded calendar grid: w^2 in [0,1], same convention as FD rollback (fixed).
    std::vector<Date> denseExpiries;
    denseExpiries.reserve(nDenseExp);
    for (Size j = 0; j < nDenseExp; ++j) {
        const Real w = (nDenseExp <= 1)
                           ? Real(0.0)
                           : static_cast<Real>(j) / static_cast<Real>(nDenseExp - 1);
        const Real frontLoaded = w * w;
        const Integer offset =
            (nDenseExp <= 1)
                ? dayFloor
                : static_cast<Integer>(std::llround(
                      dayFloor + frontLoaded * static_cast<Real>(dayHorizon - dayFloor)));
        Date d = calendar.adjust(today + offset, Following);
        if (!denseExpiries.empty() && d <= denseExpiries.back()) {
            d = calendar.adjust(denseExpiries.back() + 1, Following);
        }
        denseExpiries.push_back(d);
    }

    // dense strike grid: uniform kx on [xLow, xHigh] where xHigh is the smile right anchor minus one
    // full dense-cell step (as if the equispaced grid on [xLow, xHighRight] dropped its last node). The
    // last tabulated kx sits one mesh width inside `marketXStrikes.back()` so `TabulatedKxFlatOutsideLocalVol`
    // can flat-extend at the true right edge.
    const Real xLow = marketXStrikes.front();
    const Real xHighRight = marketXStrikes.back();
    QL_REQUIRE(xHighRight > xLow, "X strike range is degenerate");
    constexpr Size nDenseStr = 200;
    const Real denseKxStepFull =
        (xHighRight - xLow) / static_cast<Real>(nDenseStr > 1 ? nDenseStr - 1 : 1);
    const Real xHigh = xHighRight - denseKxStepFull;
    QL_REQUIRE(xHigh > xLow,
               "Buehler fixed LV: kx range degenerate after right shrink by one dense cell (increase nDenseStr "
               "or widen bicubic kx segment)");
    std::vector<Real> denseXStrikes;
    denseXStrikes.reserve(nDenseStr);
    for (Size i = 0; i < nDenseStr; ++i) {
        denseXStrikes.push_back(xLow + i * (xHigh - xLow) / (nDenseStr - 1));
    }

    // sample local vol from rebuilt X-implied surface
    Size nStr = denseXStrikes.size();
    const Size nExp = denseExpiries.size();
    auto sampledLocalVolMatrix = ext::make_shared<Matrix>(nStr, nExp);
    Size blackFallbackCount = 0;
    auto xCurve = ext::make_shared<FlatForward>(today, 0.0, dayCounter);
    xCurve->enableExtrapolation();
    Handle<YieldTermStructure> xTs(xCurve);
    auto xSpotQuote = ext::make_shared<SimpleQuote>(1.0);
    Handle<Quote> xSpot(xSpotQuote);
    auto localVolSource = ext::make_shared<LocalVolSurface>(rebuiltPureBlackVolTs, xTs, xTs, xSpot);
    localVolSource->enableExtrapolation();


    for (Size i = 0; i < nStr; ++i) {
        for (Size j = 0; j < nExp; ++j) {
            Volatility sigma = Null<Volatility>();
            try {
                sigma = localVolSource->localVol(denseExpiries[j], denseXStrikes[i], true);
            } catch (...) {
                sigma = Null<Volatility>();
            }
            const Volatility kDupireLocalVolBlackFallback = 10.0;
            if (!isPositiveFiniteVol(sigma) || sigma > kDupireLocalVolBlackFallback) {
                ++blackFallbackCount;
                const Date& d = denseExpiries[j];
                const Real kx = denseXStrikes[i];
                sigma = safeBlackVol(rebuiltPureBlackVolTs, d, kx);
                if (!isPositiveFiniteVol(sigma))
                    sigma = safeBlackVol(pureBlackVolTs, d, kx);
                if (!isPositiveFiniteVol(sigma)) {
                    const Size jm = nearestMarketExpiryIndex(marketExpiries, d);
                    sigma = impliedVolXFromGrid(impliedVolsX, marketXStrikes, jm, kx);
                }
            }
            QL_REQUIRE(isPositiveFiniteVol(sigma),
                       "Buehler Dupire dense LV: invalid σ after Black-vol fallback (check surface / grid)");
            (*sampledLocalVolMatrix)[i][j] = sigma;
        }
    }

    // When smile injection defines synthetic kx(K_min), FixedLocalVol uses only kx >= that node.
    if (useInjectedMinStrikeKxForSmile && injectedMinStrikeKxForSmile > 0.0 &&
        std::isfinite(injectedMinStrikeKxForSmile)) {
        const Real kxSyn = injectedMinStrikeKxForSmile;
        Size i0 = nStr;
        for (Size i = 0; i < nStr; ++i) {
            if (denseXStrikes[i] + 1.0e-12 >= kxSyn) {
                i0 = i;
                break;
            }
        }
        QL_REQUIRE(i0 < nStr,
                   "Synthetic kx lies above Dupire dense kx grid: cannot build fixed LV from kx_syn");
        QL_REQUIRE(nStr - i0 >= 2,
                   "Dupire kx grid leaves fewer than 2 nodes from synthetic to kx max; refine kx grid");
        std::vector<Real> croppedX(denseXStrikes.begin() + static_cast<std::ptrdiff_t>(i0),
                                   denseXStrikes.end());
        auto croppedMat = ext::make_shared<Matrix>(nStr - i0, nExp);
        for (Size ii = 0; ii < nStr - i0; ++ii) {
            for (Size jj = 0; jj < nExp; ++jj)
                (*croppedMat)[ii][jj] = (*sampledLocalVolMatrix)[i0 + ii][jj];
        }
        denseXStrikes = std::move(croppedX);
        sampledLocalVolMatrix = croppedMat;
        nStr = denseXStrikes.size();
    }

    outDenseLocalVolX = *sampledLocalVolMatrix;

    auto fixedLocalVolSurface = ext::make_shared<FixedLocalVolSurface>(
        today, denseExpiries, denseXStrikes, sampledLocalVolMatrix, dayCounter,
        FixedLocalVolSurface::ConstantExtrapolation,
        FixedLocalVolSurface::ConstantExtrapolation);
    fixedLocalVolSurface->enableExtrapolation();
    const Real kTabLo = denseXStrikes.front();
    const Real kTabHi = denseXStrikes.back();
    auto flatOutside = ext::make_shared<TabulatedKxFlatOutsideLocalVol>(
        today, dayCounter, Handle<LocalVolTermStructure>(fixedLocalVolSurface), kTabLo, kTabHi);
    outDenseExpiries = denseExpiries;
    outDenseXStrikes = denseXStrikes;
    if (outRepair != nullptr) {
        outRepair->denseGridCells = nStr * nExp;
        outRepair->dupireBlackFallbacks = blackFallbackCount;
    }
    return Handle<LocalVolTermStructure>(flatOutside);
}

/** @brief Black vol in pure X from the market Black surface (file-local). */
class BuehlerPureBlackVolSurface : public BlackVolTermStructure {
public:
    BuehlerPureBlackVolSurface(
        const Date& referenceDate,
        const Calendar& calendar,
        const DayCounter& dayCounter,
        const Handle<YieldTermStructure>& riskFreeTs,
        const Handle<YieldTermStructure>& repoTs,
        const Real spot,
        const Handle<BlackVolTermStructure>& baseBlackVolTs,
        const Date& maxBuehlerDate,
        std::vector<Time> times,
        std::vector<Real> slopes,
        std::vector<Real> intercepts)
    : BlackVolTermStructure(referenceDate, calendar, Following, dayCounter),
      riskFreeTs_(riskFreeTs), repoTs_(repoTs), spot_(spot),
      baseBlackVolTs_(baseBlackVolTs), maxBuehlerDate_(maxBuehlerDate),
      times_(std::move(times)), slopes_(std::move(slopes)), intercepts_(std::move(intercepts)) {
        QL_REQUIRE(!riskFreeTs_.empty(),     "Pure vol surface requires riskFreeTs");
        QL_REQUIRE(!repoTs_.empty(),         "Pure vol surface requires repoTs");
        QL_REQUIRE(spot_ > 0.0,              "Pure vol surface requires positive spot");
        QL_REQUIRE(!baseBlackVolTs_.empty(), "Pure vol surface requires a valid base BlackVol TS");
        QL_REQUIRE(times_.size() == slopes_.size() && slopes_.size() == intercepts_.size(),
                   "BuehlerPureBlackVolSurface: invalid grid sizes");
        QL_REQUIRE(!times_.empty(), "BuehlerPureBlackVolSurface requires non-empty time grid");
    }

    Date maxDate()   const override { return std::min(maxBuehlerDate_, baseBlackVolTs_->maxDate()); }
    Real minStrike() const override { return 0.0; }
    Real maxStrike() const override { return QL_MAX_REAL; }

protected:
    Volatility blackVolImpl(Time t, Real xStrike) const override {
        const Time clampedT = std::max(0.0, std::min(t, times_.back()));
        const auto [a, b] = affineAt(clampedT);
        QL_REQUIRE(a > 0.0, "Pure implied surface requires positive affine slope");
        QL_REQUIRE(xStrike > 0.0 && std::isfinite(xStrike), "Pure implied surface requires positive finite X strike");
        const Real kx = xStrike;
        const Real ks = a * kx + b;
        QL_REQUIRE(ks > 0.0 && std::isfinite(ks), "Pure implied surface requires positive finite S strike");
        if (clampedT < 1.0e-10)
            return baseBlackVolTs_->blackVol(clampedT, ks, true);
        const Real discount = riskFreeTs_->discount(clampedT, true);
        QL_REQUIRE(discount > 0.0, "Pure implied surface requires positive risk-free discount");
        const Real forwardS   = a + b;
        const Volatility sigmaS  = baseBlackVolTs_->blackVol(clampedT, ks, true);
        const Real stdDevS    = sigmaS * std::sqrt(clampedT);
        const Real callPriceS = blackFormula(QuantLib::Option::Call, ks, forwardS, stdDevS, discount);
        const Real callPriceXNormalized = callPriceS / (discount * a);
        const Real intrinsicX = std::max(1.0 - kx, 0.0);
        static constexpr double kNormCallArbEps = 1.0e-8;
        const Real lo = intrinsicX + static_cast<Real>(kNormCallArbEps);
        const Real hi = 1.0 - static_cast<Real>(kNormCallArbEps);
        Real callNorm = callPriceXNormalized;
        if (callNorm <= intrinsicX) {
            QL_REQUIRE(intrinsicX - callNorm < static_cast<Real>(kNormCallArbEps),
                       "Pure X implied map: normalized call price below intrinsic by more than tolerance");
            callNorm = lo;
        } else if (callNorm >= 1.0) {
            QL_REQUIRE(callNorm - 1.0 < static_cast<Real>(kNormCallArbEps),
                       "Pure X implied map: normalized call price above 1 by more than tolerance");
            callNorm = hi;
        }
        const Real stdDevGuess = 0.20 * std::sqrt(clampedT);
        const Real stdDevX = blackFormulaImpliedStdDev(
            QuantLib::Option::Call, kx, 1.0, callNorm, 1.0, 0.0, stdDevGuess, 1.0e-8, 200);
        return stdDevX / std::sqrt(clampedT);
    }

    Real blackVarianceImpl(Time t, Real xStrike) const override {
        const Volatility sigma = blackVolImpl(t, xStrike);
        return sigma * sigma * std::max<Time>(0.0, t);
    }

private:
    std::pair<Real, Real> affineAt(Time t) const {
        auto it = std::lower_bound(times_.begin(), times_.end(), t);
        if (it == times_.begin()) return {slopes_.front(), intercepts_.front()};
        if (it == times_.end())   return {slopes_.back(),  intercepts_.back()};
        const Size right = static_cast<Size>(std::distance(times_.begin(), it));
        const Size left  = right - 1;
        const Time t0 = times_[left], t1 = times_[right];
        const Real w  = (t1 > t0) ? (t - t0) / (t1 - t0) : Real(0.0);
        return {(1.0-w)*slopes_[left]     + w*slopes_[right],
                (1.0-w)*intercepts_[left] + w*intercepts_[right]};
    }

    Handle<YieldTermStructure> riskFreeTs_, repoTs_;
    Real spot_;
    Handle<BlackVolTermStructure> baseBlackVolTs_;
    Date maxBuehlerDate_;
    std::vector<Time> times_;
    std::vector<Real> slopes_, intercepts_;
};

} // namespace

BuehlerModel::BuehlerModel(MarketData& marketData)
: BuehlerModel(static_cast<const MarketData&>(marketData)) {}

BuehlerModel::BuehlerModel(const MarketData& marketData)
: today_(marketData.today()),
  maturity_(marketData.marketHorizon()),
  calendar_(marketData.calendar()),
  dayCounter_(marketData.dayCounter()),
  inputSpotValue_(marketData.spotValue()),
  inputRiskFreeTs_(marketData.riskFreeTs()),
  inputRepoTs_(marketData.repoTs()),
  inputBlackVolTs_(marketData.blackVolTs()),
  inputDividendDates_(marketData.dividendDates()),
  inputDividendAmounts_(marketData.dividendAmounts()),
  inputDividendProportional_(marketData.dividendProportional()),
  inputStrikes_(marketData.strikes()),
  inputExpiries_(marketData.expiries()),
  inputRiskFreeDates_(marketData.riskFreeDates()),
  inputRiskFreeZeroRates_(marketData.riskFreeZeroRates()),
  inputRepoDates_(marketData.repoDates()),
  inputRepoZeroRates_(marketData.repoZeroRates()),
  inputImpliedVolsMarketS_(marketData.impliedVols()) {
    QL_REQUIRE(!marketData.riskFreeTs().empty() && !marketData.repoTs().empty()
                   && !marketData.blackVolTs().empty(),
               "BuehlerModel(const MarketData&): market not ready; construct "
               "MarketData empty; call loadFromTables, loadSampleMarketSnapshot, or loadConstantMock first.");
    BuehlerBergomiParams bergomi;
    bergomi.k = marketData.bergomiK();
    bergomi.nu = marketData.bergomiNu();
    bergomi.rho = marketData.bergomiRho();
    setBergomiParams(bergomi);
    QL_REQUIRE(inputRiskFreeDates_.size() == inputRiskFreeZeroRates_.size(),
               "BuehlerModel: risk-free dates / zero rates size mismatch");
    QL_REQUIRE(inputRepoDates_.size() == inputRepoZeroRates_.size(),
               "BuehlerModel: repo dates / zero rates size mismatch");
    QL_REQUIRE(inputDividendDates_.size() == inputDividendAmounts_.size(),
               "BuehlerModel: dividend dates / cash size mismatch (market)");
    QL_REQUIRE(inputDividendDates_.size() == inputDividendProportional_.size(),
               "BuehlerModel: dividend dates / proportional size mismatch (market)");
    QL_REQUIRE(inputImpliedVolsMarketS_.rows() == inputStrikes_.size()
                   && inputImpliedVolsMarketS_.columns() == inputExpiries_.size(),
               "BuehlerModel: market implied vol matrix vs strike/expiry grid mismatch");
}

const BuehlerBergomiParams& BuehlerModel::bergomiParams() const {
    QL_REQUIRE(bergomiParams_.has_value(), "BuehlerModel: Bergomi params not set");
    return *bergomiParams_;
}

void BuehlerModel::setBergomiParams(const BuehlerBergomiParams& params) {
    QL_REQUIRE(params.k > 0.0, "BuehlerBergomiParams: k must be positive");
    QL_REQUIRE(params.nu > 0.0, "BuehlerBergomiParams: nu must be positive");
    QL_REQUIRE(params.rho > -1.0 && params.rho < 1.0,
               "BuehlerBergomiParams: rho must be in (-1, 1)");
    bergomiParams_ = params;
}

void BuehlerModel::preprocessing() {
    using namespace QuantLib;

    preBicubicImpliedVolXExpiries_.clear();
    preBicubicImpliedVolXKxGrid_.clear();
    preBicubicImpliedVolsX_ = Matrix();
    calibrationMinKx_ = Null<Real>();
    impliedVolXBicubicTs_ = Handle<BlackVolTermStructure>();

    QL_REQUIRE(!inputRiskFreeTs_.empty(),  "BuehlerModel requires riskFreeTs");
    QL_REQUIRE(!inputRepoTs_.empty(),      "BuehlerModel requires repoTs");
    QL_REQUIRE(!inputBlackVolTs_.empty(),  "BuehlerModel requires blackVolTs");
    QL_REQUIRE(maturity_ >= today_,               "BuehlerModel requires maturity >= today");
    QL_REQUIRE(inputDividendDates_.size() == inputDividendAmounts_.size(),
               "Dividend dates/cash size mismatch");
    QL_REQUIRE(inputDividendDates_.size() == inputDividendProportional_.size(),
               "Dividend dates/proportional size mismatch");

    const auto proportionalProduct = [](const std::vector<Date>& dates,
                                        const std::vector<Real>& proportional,
                                        const Date& fromExclusive, const Date& toInclusive) {
        Real product = 1.0;
        for (Size i = 0; i < dates.size(); ++i) {
            const Date tau = dates[i];
            if (tau > fromExclusive && tau <= toInclusive && proportional[i] > 0.0) {
                QL_REQUIRE(proportional[i] < 1.0,
                           "Proportional dividend must be in [0, 1)");
                product *= (1.0 - proportional[i]);
            }
        }
        return product;
    };

    businessDates_.clear();
    businessTimes_.clear();
    forwards0T_.clear();
    dividends0T_.clear();
    pureSlopes_.clear();
    pureIntercepts_.clear();

    for (Date d = today_; d <= maturity_; d = d + 1)
        if (calendar_.isBusinessDay(d))
            businessDates_.push_back(d);
    if (businessDates_.empty())
        businessDates_.push_back(today_);

    businessTimes_.reserve(businessDates_.size());
    forwards0T_.reserve(businessDates_.size());
    dividends0T_.reserve(businessDates_.size());
    pureSlopes_.reserve(businessDates_.size());
    pureIntercepts_.reserve(businessDates_.size());

    const auto& riskFreeTs      = inputRiskFreeTs_;
    const auto& repoTs          = inputRepoTs_;
    const auto& dividendDates        = inputDividendDates_;
    const auto& dividendCash         = inputDividendAmounts_;
    const auto& dividendProportional   = inputDividendProportional_;
    const Real  spot                   = inputSpotValue_;

    for (const Date& t : businessDates_) {
        const Time businessT = dayCounter_.yearFraction(today_, t);
        businessTimes_.push_back(std::max<Time>(0.0, businessT));

        const Real grossForward =
            carryGrowthFactor(riskFreeTs, repoTs, today_, t) * spot *
            proportionalProduct(dividendDates, dividendProportional, today_, t);
        Real paidDividendCarryToT    = 0.0;
        Real futureDividendEscrowAtT = 0.0;
        for (Size i = 0; i < dividendDates.size(); ++i) {
            const Date tau = dividendDates[i];
            const Real cash = dividendCash[i];
            if (tau > today_ && tau <= t) {
                if (cash > 0.0) {
                    paidDividendCarryToT +=
                        carryGrowthFactor(riskFreeTs, repoTs, tau, t) *
                        proportionalProduct(dividendDates, dividendProportional, tau, t) * cash;
                }
            } else if (tau > t) {
                if (cash > 0.0) {
                    futureDividendEscrowAtT +=
                        cash / carryGrowthFactor(riskFreeTs, repoTs, t, tau) *
                        proportionalProduct(dividendDates, dividendProportional, t, tau);
                }
            }
        }

        const Real forward0T = grossForward - paidDividendCarryToT;
        const Real intercept = futureDividendEscrowAtT;
        const Real slope     = forward0T - intercept;


        forwards0T_.push_back(forward0T);
        dividends0T_.push_back(futureDividendEscrowAtT);
        pureSlopes_.push_back(slope);
        pureIntercepts_.push_back(intercept);
    }

    auto pureVolSurface = ext::make_shared<BuehlerPureBlackVolSurface>(
        today_, calendar_, dayCounter_,
        inputRiskFreeTs_, inputRepoTs_, inputSpotValue_,
        inputBlackVolTs_, businessDates_.back(),
        businessTimes_, pureSlopes_, pureIntercepts_);
    pureVolSurface->enableExtrapolation();
    pureBlackVolTs_ = Handle<BlackVolTermStructure>(pureVolSurface);
}

void BuehlerModel::calibration(const bool runValidation) {
    using namespace QuantLib;

    fixingSavePath_.reset();
    fixingPathSimulationDates_.clear();
    lastLvDenseRepair_ = BuehlerLvDenseRepairCounts{};
    impliedVolXBicubicTs_ = Handle<BlackVolTermStructure>();

    // kx band for bicubic: common equispaced axis [segLo, segHi]. Left/right anchored like
    // the lowest/highest market strikes mapped to kx: min_T kx(K_min,T) and max_T kx(K_max,T).
    // K_min is excluded from bicubicMarketKs for the per-column smile; segLo still anchors the
    // equispaced kx grid. Additionally max_T kx(K_min,T) over bicubic expiries is injected as a
    // fixed abscissa on every smile before the monotonic cubic (see buildFixedLocalVolFromPureImpliedX).
    const std::vector<Real>& marketKs = inputStrikes_;
    const std::vector<Date>& expiries  = inputExpiries_;
    const Size nKs  = marketKs.size();
    const Size nExp = expiries.size();
    QL_REQUIRE(nKs >= 3,
               "Fixed X local-vol needs at least three market strikes after removing the lowest");
    const Size minStrikeIdx = static_cast<Size>(
        std::distance(marketKs.begin(), std::min_element(marketKs.begin(), marketKs.end())));
    std::vector<Real> bicubicMarketKs;
    bicubicMarketKs.reserve(nKs - 1);
    for (Size i = 0; i < nKs; ++i) {
        if (i != minStrikeIdx)
            bicubicMarketKs.push_back(marketKs[i]);
    }
    QL_REQUIRE(bicubicMarketKs.size() >= 2,
               "Fixed X local-vol needs at least two strikes after removing the lowest");
    const Size nKsBic = bicubicMarketKs.size();

    Real kxGlobalMin = QL_MAX_REAL;
    Real kxGlobalMax = -QL_MAX_REAL;
    for (Size i = 0; i < nKsBic; ++i) {
        for (Size j = 0; j < nExp; ++j) {
            const Real A = interpolateByDate(pureSlopes_,     expiries[j]);
            const Real D = interpolateByDate(pureIntercepts_, expiries[j]);
            if (A <= 0.0)
                continue;
            const Real kx = (bicubicMarketKs[i] - D) / A;
            if (kx > 0.0 && std::isfinite(kx)) {
                kxGlobalMin = std::min(kxGlobalMin, kx);
                kxGlobalMax = std::max(kxGlobalMax, kx);
            }
        }
    }
    QL_REQUIRE(kxGlobalMax > kxGlobalMin && std::isfinite(kxGlobalMin) &&
                   std::isfinite(kxGlobalMax),
               "No valid kx samples from market strikes and expiries");

    // Equispaced kx grid: symmetric anchors from K_min and K_max (min / max over expiries).
    const Real anchorKLo = *std::min_element(marketKs.begin(), marketKs.end());
    Real kxLeftAnchor = QL_MAX_REAL;
    for (Size j = 0; j < nExp; ++j) {
        const Real A = interpolateByDate(pureSlopes_, expiries[j]);
        const Real D = interpolateByDate(pureIntercepts_, expiries[j]);
        if (A <= 0.0)
            continue;
        const Real kx = (anchorKLo - D) / A;
        if (kx > 0.0 && std::isfinite(kx))
            kxLeftAnchor = std::min(kxLeftAnchor, kx);
    }
    QL_REQUIRE(kxLeftAnchor < QL_MAX_REAL && std::isfinite(kxLeftAnchor),
               "No valid kx anchor from lowest market strike for bicubic kx grid");

    const Real anchorKHi = *std::max_element(marketKs.begin(), marketKs.end());
    Real kxRightAnchor = -QL_MAX_REAL;
    for (Size j = 0; j < nExp; ++j) {
        const Real A = interpolateByDate(pureSlopes_, expiries[j]);
        const Real D = interpolateByDate(pureIntercepts_, expiries[j]);
        if (A <= 0.0)
            continue;
        const Real kx = (anchorKHi - D) / A;
        if (kx > 0.0 && std::isfinite(kx))
            kxRightAnchor = std::max(kxRightAnchor, kx);
    }
    QL_REQUIRE(kxRightAnchor > -QL_MAX_REAL && std::isfinite(kxRightAnchor),
               "No valid kx anchor from highest market strike for bicubic kx grid");

    const Real segLo = kxLeftAnchor;
    const Real segHi = kxRightAnchor;
    QL_REQUIRE(segHi > segLo && std::isfinite(segLo) && std::isfinite(segHi),
               "Implied-X common kx segment [kmax(min), kmin(max)] is degenerate");

    const Size nGridX = std::max<Size>(2, nKsBic);
    std::vector<Real> marketXStrikes;
    marketXStrikes.reserve(nGridX);
    for (Size i = 0; i < nGridX; ++i) {
        const Real w = (nGridX <= 1) ? Real(0.0)
                                     : static_cast<Real>(i) / static_cast<Real>(nGridX - 1);
        marketXStrikes.push_back(segLo + w * (segHi - segLo));
    }

    std::vector<Real> affA(nExp), affD(nExp);
    for (Size j = 0; j < nExp; ++j) {
        affA[j] = interpolateByDate(pureSlopes_, expiries[j]);
        affD[j] = interpolateByDate(pureIntercepts_, expiries[j]);
    }

    const Date earliestExpiry = *std::min_element(expiries.begin(), expiries.end());
    std::vector<Date> bicubicExpiries;
    std::vector<Real> bicubicAffA, bicubicAffD;
    bicubicExpiries.reserve(nExp);
    bicubicAffA.reserve(nExp);
    bicubicAffD.reserve(nExp);
    for (Size j = 0; j < nExp; ++j) {
        // Drop earliest pillar only if T < 3M (avoid too-short noisy maturities).
        if (expiries[j] == earliestExpiry &&
            dayCounter_.yearFraction(today_, earliestExpiry) < 0.25)
            continue;
        bicubicExpiries.push_back(expiries[j]);
        bicubicAffA.push_back(affA[j]);
        bicubicAffD.push_back(affD[j]);
    }
    QL_REQUIRE(bicubicExpiries.size() >= 2,
               "Fixed X local-vol bicubic needs at least two expiries after removing earliest");
    const std::vector<Real> bicubicXStrikes = marketXStrikes;

    // Shared kx abscissa for smile columns: max over bicubic expiries of kx from lowest S strike K_min.
    Real kxInjectedSmile = -QL_MAX_REAL;
    for (Size j = 0; j < bicubicAffA.size(); ++j) {
        const Real A = bicubicAffA[j];
        const Real D = bicubicAffD[j];
        if (A <= 0.0)
            continue;
        const Real kx = (anchorKLo - D) / A;
        if (kx > 0.0 && std::isfinite(kx))
            kxInjectedSmile = std::max(kxInjectedSmile, kx);
    }
    const bool useKxInjectedSmile =
        kxInjectedSmile > 0.0 && std::isfinite(kxInjectedSmile);

    calibrationMinKx_ =
        useKxInjectedSmile ? kxInjectedSmile : marketXStrikes.front();

    fixedPureLocalVolTs_ = buildFixedLocalVolFromPureImpliedX(
        today_, calendar_, dayCounter_,
        businessDates_.back(),
        pureBlackVolTs_,
        bicubicExpiries,
        bicubicXStrikes,
        bicubicMarketKs,
        bicubicAffA,
        bicubicAffD,
        denseExpiries_,
        denseXStrikes_,
        denseLocalVolXGrid_,
        &preBicubicImpliedVolsX_,
        &preBicubicImpliedVolXExpiries_,
        &preBicubicImpliedVolXKxGrid_,
        &lastLvDenseRepair_,
        &impliedVolXBicubicTs_,
        useKxInjectedSmile,
        useKxInjectedSmile ? kxInjectedSmile : 0.0);

    mcSigmaLookupCache_.reset();

    // Dupire repair health gate: the Black-vol fallback is a benign patch for isolated
    // numerically-degenerate cells, but a large fallback share means the "local vol" is
    // mostly a disguised implied vol and no longer reprices the input surface.
    if (lastLvDenseRepair_.denseGridCells > 0) {
        const double fallbackFraction =
            static_cast<double>(lastLvDenseRepair_.dupireBlackFallbacks) /
            static_cast<double>(lastLvDenseRepair_.denseGridCells);
        QL_REQUIRE(fallbackFraction <= kDupireBlackFallbackFailFraction,
                   "BuehlerModel::calibration: " << lastLvDenseRepair_.dupireBlackFallbacks
                       << " of " << lastLvDenseRepair_.denseGridCells
                       << " dense LV cells (" << 100.0 * fallbackFraction
                       << "%) fell back to Black vol, above the "
                       << 100.0 * kDupireBlackFallbackFailFraction
                       << "% hard limit; the Dupire surface is unreliable on this snapshot");
        if (fallbackFraction > kDupireBlackFallbackWarnFraction) {
            std::cerr << "[calibration warning] Dupire Black-vol fallback on "
                      << lastLvDenseRepair_.dupireBlackFallbacks << " of "
                      << lastLvDenseRepair_.denseGridCells << " dense LV cells ("
                      << 100.0 * fallbackFraction << "%, warn level "
                      << 100.0 * kDupireBlackFallbackWarnFraction << "%)\n";
        }
    }

    if (runValidation)
        validate_calibration();
}

const BuehlerMcTimeGridSigmaLookup& BuehlerModel::mcSigmaLookup() const {
    QL_REQUIRE(!fixedPureLocalVolTs_.empty(),
               "BuehlerModel::mcSigmaLookup: empty fixed pure-X local vol (run calibration)");
    if (!mcSigmaLookupCache_) {
        mcSigmaLookupCache_ = std::make_shared<const BuehlerMcTimeGridSigmaLookup>(
            fixedPureLocalVolTs_.currentLink(), *this);
    }
    return *mcSigmaLookupCache_;
}

QuantLib::Real BuehlerModel::calibrationMinKx() const {
    using namespace QuantLib;
    QL_REQUIRE(calibrationMinKx_ != Null<Real>(),
               "BuehlerModel: call calibration() before calibrationMinKx()");
    return calibrationMinKx_;
}

QuantLib::Real BuehlerModel::interpolateByDate(const std::vector<QuantLib::Real>& values,
                                               const QuantLib::Date& t) const {
    using namespace QuantLib;
    QL_REQUIRE(!businessDates_.empty(), "BuehlerModel has empty business-date grid");
    QL_REQUIRE(values.size() == businessDates_.size(), "BuehlerModel interpolation size mismatch");
    if (t <= businessDates_.front()) return values.front();
    if (t >= businessDates_.back())  return values.back();
    auto it = std::lower_bound(businessDates_.begin(), businessDates_.end(), t);
    const Size right = static_cast<Size>(std::distance(businessDates_.begin(), it));
    if (*it == t) return values[right];
    const Size left = right - 1;
    const Time t0 = businessTimes_[left];
    const Time t1 = businessTimes_[right];
    const Time tq = dayCounter_.yearFraction(today_, t);
    const Real w  = (t1 > t0) ? (tq - t0) / (t1 - t0) : Real(0.0);
    return (1.0 - w) * values[left] + w * values[right];
}

QuantLib::Real BuehlerModel::forward0T(const QuantLib::Date& t) const {
    return interpolateByDate(forwards0T_, t);
}

QuantLib::Real BuehlerModel::dividendCarry0T(const QuantLib::Date& t) const {
    return interpolateByDate(dividends0T_, t);
}

QuantLib::Real BuehlerModel::mapXtoS(const QuantLib::Date& t, QuantLib::Real x) const {
    const QuantLib::Real b = interpolateByDate(pureIntercepts_, t);
    const QuantLib::Real a = interpolateByDate(pureSlopes_,     t);
    return a * x + b;
}

void BuehlerModel::simulateFixingPaths(const QuantLib::Date& horizonMax,
                                       const std::vector<QuantLib::Date>& simulationDates,
                                       const BuehlerMcSettings& settings) {
    fixingPathHorizonMax_ = horizonMax;
    std::vector<QuantLib::Date> dates = simulationDates;
    if (dates.empty())
        dates = buehlerMcSimulationDatesEveryNBusinessDays(*this, horizonMax,
                                                             kDefaultMcBusinessDayStep);
    fixingPathSimulationDates_ = normalizeSimulationDates(*this, dates, horizonMax);
    fixingSavePath_ = simulateBuehlerFixingSavePath(*this, horizonMax, fixingPathSimulationDates_,
                                                    settings);
}

BuehlerFixingSavePath BuehlerModel::simulateFixingBank(const QuantLib::Date& horizonMax,
                                                       const std::vector<QuantLib::Date>& simulationDates,
                                                       const BuehlerMcSettings& settings) const {
    std::vector<QuantLib::Date> dates = simulationDates;
    if (dates.empty())
        dates = buehlerMcSimulationDatesEveryNBusinessDays(*this, horizonMax,
                                                           kDefaultMcBusinessDayStep);
    return simulateBuehlerFixingSavePath(*this, horizonMax, dates, settings);
}

const BuehlerFixingSavePath& BuehlerModel::fixingSavePath() const {
    QL_REQUIRE(fixingSavePath_.has_value(),
               "BuehlerModel: call simulateFixingPaths before accessing the save path");
    return *fixingSavePath_;
}

BuehlerFixingSavePath BuehlerModel::takeFixingSavePath() {
    QL_REQUIRE(fixingSavePath_.has_value(),
               "BuehlerModel: call simulateFixingPaths before taking the save path");
    BuehlerFixingSavePath out = std::move(*fixingSavePath_);
    fixingSavePath_.reset();
    return out;
}

const QuantLib::Date& BuehlerModel::fixingPathHorizonMax() const {
    QL_REQUIRE(hasFixingSavePath(), "BuehlerModel: no simulated fixing path");
    return fixingPathHorizonMax_;
}

const std::vector<QuantLib::Date>& BuehlerModel::fixingPathSimulationDates() const {
    QL_REQUIRE(hasFixingSavePath(), "BuehlerModel: no simulated fixing path");
    return fixingPathSimulationDates_;
}

namespace {

std::vector<QuantLib::Size> firstMiddleLastGridIndices(const std::vector<QuantLib::Size>& eligible,
                                                       const char* gridLabel) {
    QL_REQUIRE(eligible.size() >= 3,
               "validate_calibration: need at least 3 eligible " << gridLabel
                                                                 << " after filters (have "
                                                                 << eligible.size() << ")");
    std::vector<QuantLib::Size> picked;
    auto appendUnique = [&](QuantLib::Size i) {
        if (picked.empty() || picked.back() != i)
            picked.push_back(i);
    };
    appendUnique(eligible.front());
    appendUnique(eligible[eligible.size() / 2]);
    appendUnique(eligible.back());
    return picked;
}

} // namespace

BuehlerCalibrationValidationReport BuehlerModel::validate_calibration(
    BuehlerCalibrationValidationOptions options) const {
    using namespace QuantLib;

    QL_REQUIRE(!impliedVolXBicubicTs_.empty(),
               "validate_calibration: call calibration() first");
    QL_REQUIRE(!fixedPureLocalVolTs_.empty(),
               "validate_calibration: call calibration() first");
    QL_REQUIRE(!inputExpiries_.empty(), "validate_calibration: empty market expiries");
    QL_REQUIRE(!inputStrikes_.empty(), "validate_calibration: empty market strikes");
    QL_REQUIRE(!inputBlackVolTs_.empty(), "validate_calibration: empty market Black vol");

    BuehlerCalibrationValidationReport rep;

    const BuehlerImpliedVolXArbitrageReport arbRep =
        check_static_arbitrage(*this, 32, 64, -5.0e-4, -5.0e-4, false);
    rep.staticArbitrageOk = arbRep.allPassed();

    const Real kxTabLo = denseXStrikes_.empty() ? 0.0 : denseXStrikes_.front();
    const Real kxTabHi =
        denseXStrikes_.empty() ? std::numeric_limits<Real>::max() : denseXStrikes_.back();

    std::vector<Size> eligibleExpiries;
    eligibleExpiries.reserve(inputExpiries_.size());
    for (Size j = 1; j < inputExpiries_.size(); ++j) {
        const Time t = dayCounter_.yearFraction(today_, inputExpiries_[j]);
        if (t > 0.0)
            eligibleExpiries.push_back(j);
    }

    std::vector<Size> eligibleStrikes;
    eligibleStrikes.reserve(inputStrikes_.size());
    for (Size i = 0; i < inputStrikes_.size(); ++i) {
        bool ok = true;
        for (const Size j : eligibleExpiries) {
            const Date& expiry = inputExpiries_[j];
            const Real forwardS = forward0T(expiry);
            const Real D = dividendCarry0T(expiry);
            const Real A = forwardS - D;
            if (A <= 0.0) {
                ok = false;
                break;
            }
            const Real kx = (inputStrikes_[i] - D) / A;
            if (kx <= 0.0) {
                ok = false;
                break;
            }
            if (!denseXStrikes_.empty() && kx < kxTabLo - 1.0e-12) {
                ok = false;
                break;
            }
            if (!denseXStrikes_.empty() && kx > kxTabHi + 1.0e-12) {
                ok = false;
                break;
            }
        }
        if (ok)
            eligibleStrikes.push_back(i);
    }

    const std::vector<Size> expiryIndices =
        firstMiddleLastGridIndices(eligibleExpiries, "expiries");

    constexpr double kMinEuropeanCallPrice = 1.0e-4;
    /** Market European call in S: 0.1 centesimi (= 0.001). */
    constexpr double kMinMarketCallPriceS = 0.001;
    const Size shortestExpiryIdx = expiryIndices.front();
    const Date& shortestExpiry = inputExpiries_[shortestExpiryIdx];

    const auto marketEuropeanCallPriceInS = [&](const Date& expiry,
                                              const Real strikeS) -> Real {
        const Time t = dayCounter_.yearFraction(today_, expiry);
        if (t <= 0.0)
            return Null<Real>();
        const Real forwardS = forward0T(expiry);
        const Real discountS = inputRiskFreeTs_->discount(expiry);
        const Volatility sigmaS = inputBlackVolTs_->blackVol(expiry, strikeS, true);
        if (!std::isfinite(sigmaS) || sigmaS <= 0.0)
            return Null<Real>();
        const Real callPriceS = blackFormula(QuantLib::Option::Call, strikeS, forwardS,
                                             sigmaS * std::sqrt(t), discountS);
        return std::isfinite(callPriceS) ? callPriceS : Null<Real>();
    };

    std::vector<Size> priceableStrikes;
    priceableStrikes.reserve(eligibleStrikes.size());
    for (const Size i : eligibleStrikes) {
        const Real marketCallS = marketEuropeanCallPriceInS(shortestExpiry, inputStrikes_[i]);
        if (std::isfinite(marketCallS) && marketCallS > kMinMarketCallPriceS)
            priceableStrikes.push_back(i);
    }

    const std::vector<Size> strikeIndices =
        firstMiddleLastGridIndices(priceableStrikes, "strikes");
    const Size expectedSamples = expiryIndices.size() * strikeIndices.size();
    rep.expectedSmileFitSamples = expectedSamples;
    rep.priceableStrikeCount = priceableStrikes.size();
    rep.probeShortestExpiry = shortestExpiry;
    rep.smileFitCells.reserve(expectedSamples);

    double sumAbsErrIvBp = 0.0;

    for (const Size j : expiryIndices) {
        const Date& expiry = inputExpiries_[j];
        const Time t = dayCounter_.yearFraction(today_, expiry);
        if (t <= 0.0)
            continue;

        const Real forwardS = forward0T(expiry);
        const Real discountS = inputRiskFreeTs_->discount(expiry);

        for (const Size i : strikeIndices) {
            const Real strikeS = inputStrikes_[i];
            BuehlerCalibrationSmileFitSample cell;
            cell.expiry = expiry;
            cell.strikeS = strikeS;

            const Real D = dividendCarry0T(expiry);
            const Real A = forwardS - D;
            if (A <= 0.0) {
                cell.status = "skipped_affine";
                rep.smileFitCells.push_back(cell);
                continue;
            }

            const Real kx = (strikeS - D) / A;
            if (kx <= 0.0) {
                cell.status = "skipped_kx";
                rep.smileFitCells.push_back(cell);
                continue;
            }

            OptionContractParams params;
            params.expiry = expiry;
            params.strike = strikeS;
            params.isCall = true;

            const LvEuropeanFdBuehlerOption option(params, BuehlerOptionPriceSpace::S,
                                                   options.fdTGridPerYear, options.fdXGrid);
            Real lvPriceS = Null<Real>();
            try {
                lvPriceS = option.price(*this);
            } catch (const std::exception&) {
                cell.status = "fd_failed";
                rep.smileFitCells.push_back(cell);
                continue;
            }
            cell.lvPriceS = lvPriceS;
            if (!std::isfinite(lvPriceS) || lvPriceS <= kMinEuropeanCallPrice) {
                cell.status = "lv_price_too_low";
                rep.smileFitCells.push_back(cell);
                continue;
            }

            const Volatility sigmaMarketS = inputBlackVolTs_->blackVol(expiry, strikeS, true);
            cell.sigmaMarketS = sigmaMarketS;
            if (!std::isfinite(sigmaMarketS) || sigmaMarketS <= 0.0) {
                cell.status = "bad_market_vol";
                rep.smileFitCells.push_back(cell);
                continue;
            }

            const Real stdDevMkt = sigmaMarketS * std::sqrt(t);
            try {
                const Real stdDevImp = blackFormulaImpliedStdDev(
                    QuantLib::Option::Call, strikeS, forwardS, lvPriceS, discountS, 0.0, stdDevMkt,
                    1.0e-8, 200);
                const double sigmaImpS = static_cast<double>(stdDevImp / std::sqrt(t));
                if (!std::isfinite(sigmaImpS) || sigmaImpS <= 0.0) {
                    cell.status = "inversion_bad_sigma";
                    rep.smileFitCells.push_back(cell);
                    continue;
                }
                cell.sigmaImpS = static_cast<Real>(sigmaImpS);
                cell.absErrIvBp =
                    10000.0 * std::fabs(sigmaImpS - static_cast<double>(sigmaMarketS));
                cell.status = "ok";
                sumAbsErrIvBp += cell.absErrIvBp;
                ++rep.smileFitSamples;
            } catch (const std::exception&) {
                cell.status = "inversion_failed";
            }
            rep.smileFitCells.push_back(cell);
        }
    }

    if (rep.smileFitSamples == 0) {
        if (options.throwOnFailure) {
            QL_FAIL("validate_calibration: no smile-fit samples on selected expiries/strikes");
        }
        rep.meanAbsIvErrBp = 0.0;
        rep.smileFitOk = false;
    } else {
        rep.meanAbsIvErrBp = sumAbsErrIvBp / static_cast<double>(rep.smileFitSamples);
        rep.smileFitOk = rep.smileFitSamples == expectedSamples &&
                         rep.meanAbsIvErrBp <= options.meanIvErrBpThreshold;
    }

    if (rep.smileFitSamples != expectedSamples && options.throwOnFailure) {
        std::ostringstream oss;
        oss << "validate_calibration: expected " << expectedSamples
            << " smile-fit samples but got " << rep.smileFitSamples;
        QL_FAIL(oss.str());
    }

    if (options.verbose) {
        std::cout << "static arbitrage: " << (rep.staticArbitrageOk ? "PASS" : "FAIL") << '\n';
        std::cout << "fit implied vol: " << (rep.smileFitOk ? "PASS" : "FAIL") << '\n';
    }

    if (!rep.staticArbitrageOk && options.throwOnFailure) {
        std::ostringstream oss;
        oss << "validate_calibration: static arbitrage failed"
            << " (butterfly=" << arbRep.violationsButterfly
            << " calendar=" << arbRep.violationsCalendar << ")";
        QL_FAIL(oss.str());
    }

    if (!rep.smileFitOk && options.throwOnFailure) {
        std::ostringstream oss;
        oss << "validate_calibration: mean IV error " << rep.meanAbsIvErrBp
            << " bp exceeds threshold " << options.meanIvErrBpThreshold << " bp (samples="
            << rep.smileFitSamples << ")";
        QL_FAIL(oss.str());
    }

    return rep;
}