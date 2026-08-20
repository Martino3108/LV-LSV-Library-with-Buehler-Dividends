/**
 * @file buehler_model.cpp
 * @brief BuehlerModel implementation and file-local helpers.
 */

#include "buehler_model.h"
#include "buehler_fixing_path_simulate.h"
#include "buehler_iv_x_arbitrage.h"
#include "buehler_mc_sigma_lookup.h"
#include "market_data.h"
#include "linear_time_cubic_strike_interpolation.h"
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

/** Linear in kx on nodal implied σ_X (monotonic smile columns). */
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

/** @brief X-implied vol grid, lin-T/cubic-k wrap, Dupire LV → `FixedLocalVolSurface`. */
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
    Matrix* nodalImpliedVolsXOut = nullptr,
    std::vector<Date>* nodalExpiriesOut = nullptr,
    std::vector<Real>* nodalXStrikesOut = nullptr,
    BuehlerLvDenseRepairCounts* outRepair = nullptr,
    Handle<BlackVolTermStructure>* impliedVolXOut = nullptr,
    bool useInjectedMinStrikeKxForSmile = false,
    Real injectedMinStrikeKxForSmile = 0.0,
    bool useInjectedMaxStrikeKxForSmile = false,
    Real injectedMaxStrikeKxForSmile = 0.0) {

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

    // Implied σ_X: map market strikes K→kx (exact S nodes via the wrapper), fit a
    // monotonic cubic in total variance w=σ²T on that irregular kx support, then
    // evaluate on the common equispaced kx grid (linear wings in w outside support).
    // Synthetic kx bounds (injected* flags below) are crop markers only — they are
    // not sampled via S; off-support equispaced nodes come from the X cubic / wings.
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

        const Time Texp = dayCounter.yearFraction(today, marketExpiries[j]);
        QL_REQUIRE(Texp > 0.0, "Buehler implied σ_X: non-positive expiry year fraction");

        std::vector<Real> kxs;
        std::vector<Real> sigs;
        kxs.reserve(nKmer);
        sigs.reserve(nKmer);
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

        std::vector<Real> uw(uy.size());
        for (Size t = 0; t < uy.size(); ++t)
            uw[t] = uy[t] * uy[t] * Texp;
        MonotonicCubicNaturalSpline smileInterp(ux.begin(), ux.end(), uw.begin());
        const Real kxSupportLo = ux.front();
        const Real kxSupportHi = ux.back();
        const Real wLo = smileInterp(kxSupportLo, false);
        const Real wHi = smileInterp(kxSupportHi, false);
        const Real derLo = smileInterp.derivative(kxSupportLo, false);
        const Real derHi = smileInterp.derivative(kxSupportHi, false);
        const auto sigmaFromW = [&](Real w) -> Volatility {
            if (!(w > 0.0) || !std::isfinite(w))
                return Null<Volatility>();
            return std::sqrt(w / Texp);
        };
        for (Size i = 0; i < marketXStrikes.size(); ++i) {
            const Real kxTgt = marketXStrikes[i];
            Real w;
            if (kxTgt < kxSupportLo) {
                QL_REQUIRE(wLo > 0.0 && std::isfinite(wLo) && std::isfinite(derLo),
                           "left linear wing requires positive finite w and derivative at kx_min");
                w = wLo + (kxTgt - kxSupportLo) * derLo;
            } else if (kxTgt > kxSupportHi) {
                QL_REQUIRE(wHi > 0.0 && std::isfinite(wHi) && std::isfinite(derHi),
                           "right linear wing requires positive finite w and derivative at kx_max");
                w = wHi + (kxTgt - kxSupportHi) * derHi;
            } else
                w = smileInterp(kxTgt, false);
            Volatility sigma = sigmaFromW(w);
            if (!std::isfinite(sigma) || sigma <= 0.0)
                sigma = uy.front();
            QL_REQUIRE(std::isfinite(sigma) && sigma > 0.0,
                       "Buehler implied σ_X: invalid vol after smile column repair");
            impliedVolsX[i][j] = sigma;
        }
    }

    if (nodalImpliedVolsXOut != nullptr) {
        *nodalImpliedVolsXOut = impliedVolsX;
        if (nodalExpiriesOut != nullptr)
            *nodalExpiriesOut = marketExpiries;
        if (nodalXStrikesOut != nullptr)
            *nodalXStrikesOut = marketXStrikes;
    }

    auto blackSurfaceX = ext::make_shared<BlackVarianceSurface>(
        today, calendar,
        marketExpiries, marketXStrikes,
        impliedVolsX, dayCounter,
        BlackVarianceSurface::InterpolatorDefaultExtrapolation,
        BlackVarianceSurface::InterpolatorDefaultExtrapolation);
    blackSurfaceX->setInterpolation<LinearTimeCubicStrike>();
    blackSurfaceX->enableExtrapolation();
    Handle<BlackVolTermStructure> rebuiltPureBlackVolTs(blackSurfaceX);
    if (impliedVolXOut != nullptr) {
        *impliedVolXOut = rebuiltPureBlackVolTs;
    }

    // dense time grid: floor at 1M (avoid Dupire deep in short-end extrapolation).
    // Front-loaded nodes in ACT/365 year-fraction space (same quote clock as the market),
    // mapped with dateFromAct365YearFraction — no Business/252 hybrid.
    const Date marketMaxExpiry = *std::max_element(marketExpiries.begin(), marketExpiries.end());
    const Time marketMaxT  = dayCounter.yearFraction(today, marketMaxExpiry);
    const Time timeFloor   = 1.0 / 12.0;
    const Time denseHorizonT = std::max(marketMaxT, timeFloor);
    const Size nDenseExp =
        std::max<Size>(2, static_cast<Size>(std::ceil(denseHorizonT * 12.0)));

    // Front-loaded ACT/365 grid: w^2 in [0,1], same front-loading shape as FD rollback.
    std::vector<Date> denseExpiries;
    denseExpiries.reserve(nDenseExp);
    for (Size j = 0; j < nDenseExp; ++j) {
        const Real w = (nDenseExp <= 1)
                           ? Real(0.0)
                           : static_cast<Real>(j) / static_cast<Real>(nDenseExp - 1);
        const Real frontLoaded = w * w;
        const Time t =
            (nDenseExp <= 1)
                ? timeFloor
                : timeFloor + frontLoaded * (denseHorizonT - timeFloor);
        Date d = dateFromAct365YearFraction(today, t, calendar);
        if (!denseExpiries.empty() && d <= denseExpiries.back()) {
            d = denseExpiries.back() + 1; // keep nodes strictly increasing in calendar time
        }
        denseExpiries.push_back(d);
    }

    // dense strike grid: uniform kx on the common abscissa [xLow, xHighRight], then cropped to the
    // synthetic band [max_T kx(K_min), min_T kx(K_max)] when those nodes are injected.
    const Real xLow = marketXStrikes.front();
    const Real xHighRight = marketXStrikes.back();
    QL_REQUIRE(xHighRight > xLow, "X strike range is degenerate");
    constexpr Size nDenseStr = 200;
    std::vector<Real> denseXStrikes;
    denseXStrikes.reserve(nDenseStr);
    for (Size i = 0; i < nDenseStr; ++i) {
        denseXStrikes.push_back(xLow + i * (xHighRight - xLow) / (nDenseStr - 1));
    }

    // Sample Dupire LV, then repair bad cells with the nearest good LV at a larger kx
    // (scan high→low strike per expiry). IV only if no good larger-strike donor exists.
    Size nStr = denseXStrikes.size();
    const Size nExp = denseExpiries.size();
    auto sampledLocalVolMatrix = ext::make_shared<Matrix>(nStr, nExp);
    // Per-strike-row repair tally: the health gate must compare repairs and cells
    // over the SAME region — the synthetic band the surface is cropped to below.
    // The dense axis spans [min_T kx(K_min), max_T kx(K_max)], which cross-expiry
    // carry can stretch far beyond any single expiry's quoted support; cells
    // outside the crop are discarded wholesale and must not count against the
    // gate. Counting them while dividing by the cropped cell count reported
    // fractions above 100 % on stretched axes.
    std::vector<Size> repairByRow(nStr, 0);
    auto xCurve = ext::make_shared<FlatForward>(today, 0.0, dayCounter);
    xCurve->enableExtrapolation();
    Handle<YieldTermStructure> xTs(xCurve);
    auto xSpotQuote = ext::make_shared<SimpleQuote>(1.0);
    Handle<Quote> xSpot(xSpotQuote);
    auto localVolSource = ext::make_shared<LocalVolSurface>(rebuiltPureBlackVolTs, xTs, xTs, xSpot);
    localVolSource->enableExtrapolation();

    constexpr Volatility kDupireLocalVolMax = 2.0;
    auto isUsableDupireLv = [&](Volatility sigma) {
        return isPositiveFiniteVol(sigma) && sigma <= kDupireLocalVolMax;
    };

    for (Size j = 0; j < nExp; ++j) {
        const Date& d = denseExpiries[j];
        for (Size i = 0; i < nStr; ++i) {
            Volatility sigma = Null<Volatility>();
            try {
                sigma = localVolSource->localVol(d, denseXStrikes[i], true);
            } catch (...) {
                sigma = Null<Volatility>();
            }
            (*sampledLocalVolMatrix)[i][j] = isUsableDupireLv(sigma) ? sigma : Null<Volatility>();
        }

        Volatility donor = Null<Volatility>();
        for (Size ii = nStr; ii > 0; --ii) {
            const Size i = ii - 1;
            Volatility sigma = (*sampledLocalVolMatrix)[i][j];
            if (isUsableDupireLv(sigma)) {
                donor = sigma;
                continue;
            }
            ++repairByRow[i];
            if (isUsableDupireLv(donor)) {
                (*sampledLocalVolMatrix)[i][j] = donor;
                continue;
            }
            // No good LV at larger kx: last-resort IV at this node (does not become donor).
            const Real kx = denseXStrikes[i];
            sigma = safeBlackVol(rebuiltPureBlackVolTs, d, kx);
            if (!isPositiveFiniteVol(sigma))
                sigma = safeBlackVol(pureBlackVolTs, d, kx);
            if (!isPositiveFiniteVol(sigma)) {
                const Size jm = nearestMarketExpiryIndex(marketExpiries, d);
                sigma = impliedVolXFromGrid(impliedVolsX, marketXStrikes, jm, kx);
            }
            QL_REQUIRE(isPositiveFiniteVol(sigma),
                       "Buehler Dupire dense LV: invalid σ after strike-right/IV repair "
                       "(check surface / grid)");
            (*sampledLocalVolMatrix)[i][j] = sigma;
        }
    }

    // Crop FixedLocalVol to the synthetic common band:
    //   left  = max_T kx(K_min,T),   right = min_T kx(K_max,T)
    // (specular edges of the reliable S→X strike map).
    Size i0 = 0;
    Size i1 = nStr; // exclusive
    if (useInjectedMinStrikeKxForSmile && injectedMinStrikeKxForSmile > 0.0 &&
        std::isfinite(injectedMinStrikeKxForSmile)) {
        const Real kxSynLo = injectedMinStrikeKxForSmile;
        i0 = nStr;
        for (Size i = 0; i < nStr; ++i) {
            if (denseXStrikes[i] + 1.0e-12 >= kxSynLo) {
                i0 = i;
                break;
            }
        }
        QL_REQUIRE(i0 < nStr,
                   "Synthetic kx_lo lies above Dupire dense kx grid: cannot build fixed LV");
    }
    if (useInjectedMaxStrikeKxForSmile && injectedMaxStrikeKxForSmile > 0.0 &&
        std::isfinite(injectedMaxStrikeKxForSmile)) {
        const Real kxSynHi = injectedMaxStrikeKxForSmile;
        i1 = 0;
        for (Size i = nStr; i > 0; --i) {
            if (denseXStrikes[i - 1] <= kxSynHi + 1.0e-12) {
                i1 = i;
                break;
            }
        }
        QL_REQUIRE(i1 > 0,
                   "Synthetic kx_hi lies below Dupire dense kx grid: cannot build fixed LV");
    }
    QL_REQUIRE(i1 > i0,
               "Dupire kx synthetic band [kx_lo, kx_hi] is empty after crop");
    QL_REQUIRE(i1 - i0 >= 2,
               "Dupire kx grid leaves fewer than 2 nodes in the synthetic band; refine kx grid");
    if (i0 != 0 || i1 != nStr) {
        std::vector<Real> croppedX(denseXStrikes.begin() + static_cast<std::ptrdiff_t>(i0),
                                   denseXStrikes.begin() + static_cast<std::ptrdiff_t>(i1));
        auto croppedMat = ext::make_shared<Matrix>(i1 - i0, nExp);
        for (Size ii = 0; ii < i1 - i0; ++ii) {
            for (Size jj = 0; jj < nExp; ++jj)
                (*croppedMat)[ii][jj] = (*sampledLocalVolMatrix)[i0 + ii][jj];
        }
        denseXStrikes = std::move(croppedX);
        sampledLocalVolMatrix = croppedMat;
        nStr = denseXStrikes.size();
    }

    outDenseLocalVolX = *sampledLocalVolMatrix;

    // Gate scope: repairs inside the cropped band only (rows i0 ≤ i < i1 of the
    // original axis; after a crop the matrix was rebased so the tally keeps the
    // pre-crop indices).
    Size dupireRepairCount = 0;
    for (Size i = i0; i < i0 + nStr; ++i)
        dupireRepairCount += repairByRow[i];

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
        outRepair->dupireRepairs = dupireRepairCount;
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
        const Real stdDevGuess = std::max(sigmaS, Real(1.0e-8)) * std::sqrt(clampedT);
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

    nodalImpliedVolXExpiries_.clear();
    nodalImpliedVolXKxGrid_.clear();
    nodalImpliedVolsX_ = Matrix();
    calibrationMinKx_ = Null<Real>();
    calibrationMaxKx_ = Null<Real>();
    impliedVolXTs_ = Handle<BlackVolTermStructure>();

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

    // Affine A(t)/D(t) nodes: every calendar day to maturity (same clock as ACT/365 MC).
    for (Date d = today_; d <= maturity_; d = d + 1)
        businessDates_.push_back(d);
    QL_REQUIRE(!businessDates_.empty(), "BuehlerModel: empty affine date grid");

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
        const Time yearFracT = dayCounter_.yearFraction(today_, t);
        businessTimes_.push_back(std::max<Time>(0.0, yearFracT));

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
                    // Bring α back with the same growth used on paid cash: funding
                    // carry and later proportionals. Π_{(t,τ]}(1−β) = C_e/C(t);
                    // D += α / (K(t,τ) Π) = α (K(t)/K_e) (C(t)/C_e).
                    const Real propGrowth = proportionalProduct(
                        dividendDates, dividendProportional, t, tau);
                    QL_REQUIRE(propGrowth > 0.0,
                               "Proportional product between t and cash ex-date must be positive");
                    futureDividendEscrowAtT +=
                        cash / (carryGrowthFactor(riskFreeTs, repoTs, t, tau) * propGrowth);
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
    impliedVolXTs_ = Handle<BlackVolTermStructure>();

    // kx band for σ_X surface: common equispaced axis [segLo, segHi]. Left/right anchored on
    // min_T kx(K_min,T) and max_T kx(K_max,T). K_min and K_max are excluded from
    // smileMarketKs for the per-column smile; the synthetic nodes
    // max_T kx(K_min,T) and min_T kx(K_max,T) are injected on every smile and define the
    // FixedLocalVol crop (see buildFixedLocalVolFromPureImpliedX).
    const std::vector<Real>& marketKs = inputStrikes_;
    const std::vector<Date>& expiries  = inputExpiries_;
    const Size nKs  = marketKs.size();
    const Size nExp = expiries.size();
    QL_REQUIRE(nKs >= 4,
               "Fixed X local-vol needs at least four market strikes after removing the lowest "
               "and highest");
    const Size minStrikeIdx = static_cast<Size>(
        std::distance(marketKs.begin(), std::min_element(marketKs.begin(), marketKs.end())));
    const Size maxStrikeIdx = static_cast<Size>(
        std::distance(marketKs.begin(), std::max_element(marketKs.begin(), marketKs.end())));
    QL_REQUIRE(minStrikeIdx != maxStrikeIdx,
               "Fixed X local-vol: lowest and highest market strikes coincide");
    std::vector<Real> smileMarketKs;
    smileMarketKs.reserve(nKs - 2);
    for (Size i = 0; i < nKs; ++i) {
        if (i != minStrikeIdx && i != maxStrikeIdx)
            smileMarketKs.push_back(marketKs[i]);
    }
    QL_REQUIRE(smileMarketKs.size() >= 2,
               "Fixed X local-vol needs at least two strikes after removing the lowest and highest");
    const Size nSmileKs = smileMarketKs.size();

    Real kxGlobalMin = QL_MAX_REAL;
    Real kxGlobalMax = -QL_MAX_REAL;
    for (Size i = 0; i < nSmileKs; ++i) {
        for (Size j = 0; j < nExp; ++j) {
            const Real A = interpolateByDate(pureSlopes_,     expiries[j]);
            const Real D = interpolateByDate(pureIntercepts_, expiries[j]);
            if (A <= 0.0)
                continue;
            const Real kx = (smileMarketKs[i] - D) / A;
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
               "No valid kx anchor from lowest market strike for σ_X kx grid");

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
               "No valid kx anchor from highest market strike for σ_X kx grid");

    const Real segLo = kxLeftAnchor;
    const Real segHi = kxRightAnchor;
    QL_REQUIRE(segHi > segLo && std::isfinite(segLo) && std::isfinite(segHi),
               "Implied-X common kx segment [min kx(K_min), max kx(K_max)] is degenerate");

    const Size nGridX = std::max<Size>(2, nSmileKs);
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

    std::vector<Date> surfaceExpiries = expiries;
    std::vector<Real> surfaceAffA = affA;
    std::vector<Real> surfaceAffD = affD;
    QL_REQUIRE(surfaceExpiries.size() >= 2,
               "Fixed X local-vol needs at least two market expiries");
    const std::vector<Real> surfaceXStrikes = marketXStrikes;

    // Shared synthetic kx abscissae for smile columns / FixedLocalVol crop:
    // left  = max over surface expiries of kx(K_min),
    // right = min over surface expiries of kx(K_max).
    Real kxInjectedSmileLo = -QL_MAX_REAL;
    Real kxInjectedSmileHi = QL_MAX_REAL;
    for (Size j = 0; j < surfaceAffA.size(); ++j) {
        const Real A = surfaceAffA[j];
        const Real D = surfaceAffD[j];
        if (A <= 0.0)
            continue;
        const Real kxLo = (anchorKLo - D) / A;
        if (kxLo > 0.0 && std::isfinite(kxLo))
            kxInjectedSmileLo = std::max(kxInjectedSmileLo, kxLo);
        const Real kxHi = (anchorKHi - D) / A;
        if (kxHi > 0.0 && std::isfinite(kxHi))
            kxInjectedSmileHi = std::min(kxInjectedSmileHi, kxHi);
    }
    const bool useKxInjectedSmileLo =
        kxInjectedSmileLo > 0.0 && std::isfinite(kxInjectedSmileLo);
    const bool useKxInjectedSmileHi =
        kxInjectedSmileHi < QL_MAX_REAL && kxInjectedSmileHi > 0.0 &&
        std::isfinite(kxInjectedSmileHi);
    QL_REQUIRE(useKxInjectedSmileLo && useKxInjectedSmileHi,
               "Fixed X local-vol: could not form synthetic kx band from K_min / K_max");
    QL_REQUIRE(kxInjectedSmileHi > kxInjectedSmileLo,
               "Fixed X local-vol: synthetic band max_T kx(K_min) >= min_T kx(K_max)");

    calibrationMinKx_ = kxInjectedSmileLo;
    calibrationMaxKx_ = kxInjectedSmileHi;

    fixedPureLocalVolTs_ = buildFixedLocalVolFromPureImpliedX(
        today_, calendar_, dayCounter_,
        businessDates_.back(),
        pureBlackVolTs_,
        surfaceExpiries,
        surfaceXStrikes,
        smileMarketKs,
        surfaceAffA,
        surfaceAffD,
        denseExpiries_,
        denseXStrikes_,
        denseLocalVolXGrid_,
        &nodalImpliedVolsX_,
        &nodalImpliedVolXExpiries_,
        &nodalImpliedVolXKxGrid_,
        &lastLvDenseRepair_,
        &impliedVolXTs_,
        true,
        kxInjectedSmileLo,
        true,
        kxInjectedSmileHi);

    QL_REQUIRE(!impliedVolXTs_.empty(),
               "BuehlerModel::calibration: empty rebuilt σ_X Black surface");
    // Replace the market-S wrapper: after the nodal X rebuild, σ_X lives on its own
    // surface (lin-T/cubic-K). FD/queries must not re-enter market-S interpolation.
    pureBlackVolTs_ = impliedVolXTs_;

    mcSigmaLookupCache_.reset();

    // Dupire repair health gate: strike-right LV fill is a benign patch for isolated
    // bad cells; a large fallback share means the Dupire surface is unreliable.
    if (lastLvDenseRepair_.denseGridCells > 0) {
        const double fallbackFraction =
            static_cast<double>(lastLvDenseRepair_.dupireRepairs) /
            static_cast<double>(lastLvDenseRepair_.denseGridCells);
        QL_REQUIRE(fallbackFraction <= kDupireRepairFailFraction,
                   "BuehlerModel::calibration: " << lastLvDenseRepair_.dupireRepairs
                       << " of " << lastLvDenseRepair_.denseGridCells
                       << " dense LV cells (" << 100.0 * fallbackFraction
                       << "%) required strike-right/IV repair; exceeds "
                       << 100.0 * kDupireRepairFailFraction
                       << "% hard limit; the Dupire surface is unreliable on this snapshot");
        if (fallbackFraction > kDupireRepairWarnFraction) {
            std::cerr << "[calibration warning] Dupire strike-right LV repair on "
                      << lastLvDenseRepair_.dupireRepairs << " of "
                      << lastLvDenseRepair_.denseGridCells << " dense LV cells ("
                      << 100.0 * fallbackFraction << "%, warn level "
                      << 100.0 * kDupireRepairWarnFraction << "%)\n";
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

QuantLib::Real BuehlerModel::calibrationMaxKx() const {
    using namespace QuantLib;
    QL_REQUIRE(calibrationMaxKx_ != Null<Real>(),
               "BuehlerModel: call calibration() before calibrationMaxKx()");
    return calibrationMaxKx_;
}

QuantLib::Real BuehlerModel::interpolateByDate(const std::vector<QuantLib::Real>& values,
                                               const QuantLib::Date& t) const {
    using namespace QuantLib;
    QL_REQUIRE(!businessDates_.empty(), "BuehlerModel has empty affine date grid");
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
        dates = buehlerMcSimulationDatesEveryNCalendarDays(*this, horizonMax,
                                                             kDefaultMcCalendarDayStep);
    // Ensure requested save fixings are on the evolution grid (no-op when the
    // default calendar-day schedule already covers today..horizon).
    dates.insert(dates.end(), settings.mcSavePathFixingDates.begin(),
                 settings.mcSavePathFixingDates.end());
    fixingPathSimulationDates_ = normalizeSimulationDates(*this, dates, horizonMax);
    fixingSavePath_ = simulateBuehlerFixingSavePath(*this, horizonMax, fixingPathSimulationDates_,
                                                    settings);
}

BuehlerFixingSavePath BuehlerModel::simulateFixingBank(const QuantLib::Date& horizonMax,
                                                       const std::vector<QuantLib::Date>& simulationDates,
                                                       const BuehlerMcSettings& settings) const {
    std::vector<QuantLib::Date> dates = simulationDates;
    if (dates.empty())
        dates = buehlerMcSimulationDatesEveryNCalendarDays(*this, horizonMax,
                                                           kDefaultMcCalendarDayStep);
    dates.insert(dates.end(), settings.mcSavePathFixingDates.begin(),
                 settings.mcSavePathFixingDates.end());
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

BuehlerCalibrationValidationReport BuehlerModel::validate_calibration(
    BuehlerCalibrationValidationOptions options) const {
    using namespace QuantLib;

    QL_REQUIRE(!impliedVolXTs_.empty(),
               "validate_calibration: call calibration() first");
    QL_REQUIRE(!fixedPureLocalVolTs_.empty(),
               "validate_calibration: call calibration() first");

    BuehlerCalibrationValidationReport rep;

    const BuehlerImpliedVolXArbitrageReport arbRep =
        check_static_arbitrage(*this, 240, 100, 0.0, 0.0, false);
    rep.staticArbitrageOk = arbRep.allPassed();

    if (options.verbose) {
        std::cout << "static arbitrage: " << (rep.staticArbitrageOk ? "PASS" : "FAIL") << '\n';
    }

    if (!rep.staticArbitrageOk && options.throwOnFailure) {
        std::ostringstream oss;
        oss << "validate_calibration: static arbitrage failed"
            << " (butterfly=" << arbRep.violationsButterfly << "/" << arbRep.nSamplesButterfly
            << " calendar=" << arbRep.violationsCalendar << "/" << arbRep.nSamplesCalendar
            << " minButterfly=" << arbRep.minButterfly << "; gate: >"
            << (100.0 * BuehlerImpliedVolXArbitrageReport::kMaxViolationFraction)
            << "% violations or minButterfly < "
            << BuehlerImpliedVolXArbitrageReport::kMinButterflyFloor << ")";
        QL_FAIL(oss.str());
    }

    return rep;
}