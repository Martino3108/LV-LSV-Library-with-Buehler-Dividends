/**
 * @file buehler_mc_sigma_lookup.h
 * @brief Shared σ(t,X) lookup on the calib dense grid (same as fast LV path).
 */

#ifndef BUEHLER_MC_SIGMA_LOOKUP_H
#define BUEHLER_MC_SIGMA_LOOKUP_H

#include "buehler_model.h"
#include <ql/termstructures/volatility/equityfx/localvoltermstructure.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

/**
 * σ(t,·) with the time bracketing already resolved; per-path lookup is X-only.
 * Built once per evolution step via @c BuehlerMcTimeGridSigmaLookup::sliceAtTime,
 * since t is shared by every path within a step. Arithmetic is ordered exactly as
 * @c atTime (X interp on the two time rows, then time blend) so results are bit-identical.
 */
class BuehlerMcSigmaTimeSlice {
public:
    QuantLib::Real atX(const QuantLib::Real x) const {
        const QuantLib::Real xClamped = std::min(std::max(x, kLo_), kHi_);
        const std::vector<QuantLib::Real>& strikes = *strikes_;
        const QuantLib::Size nK = strikes.size();

        // Bracket index: O(1) guess on the uniform grid, then a fix-up step so the
        // result matches std::lower_bound exactly (float division can be off by one
        // near the nodes). Falls back to binary search if the grid is not uniform.
        QuantLib::Size j1;
        if (invStep_ > 0.0) {
            const QuantLib::Real pos = (xClamped - kLo_) * invStep_;
            j1 = static_cast<QuantLib::Size>(pos) + 1;
            if (j1 > nK - 1)
                j1 = nK - 1;
            while (j1 < nK - 1 && strikes[j1] < xClamped)
                ++j1;
            while (j1 > 0 && strikes[j1 - 1] >= xClamped)
                --j1;
        } else {
            const auto kIt = std::lower_bound(strikes.begin(), strikes.end(), xClamped);
            j1 = static_cast<QuantLib::Size>(kIt - strikes.begin());
            if (j1 >= nK)
                j1 = nK - 1;
        }
        const QuantLib::Size j0 = (j1 == 0) ? 0 : j1 - 1;

        const QuantLib::Real k0 = strikes[j0];
        const QuantLib::Real k1 = strikes[j1];
        const QuantLib::Real wk = (k1 > k0) ? (xClamped - k0) / (k1 - k0) : 0.0;

        const QuantLib::Real v0 = row0_[j0] + wk * (row0_[j1] - row0_[j0]);
        const QuantLib::Real v1 = row1_[j0] + wk * (row1_[j1] - row1_[j0]);
        return v0 + wt_ * (v1 - v0);
    }

private:
    friend class BuehlerMcTimeGridSigmaLookup;
    const std::vector<QuantLib::Real>* strikes_ = nullptr;
    const QuantLib::Real* row0_ = nullptr;
    const QuantLib::Real* row1_ = nullptr;
    QuantLib::Real wt_ = 0.0;
    QuantLib::Real kLo_ = 0.0;
    QuantLib::Real kHi_ = 0.0;
    QuantLib::Real invStep_ = 0.0;
};

/**
 * σ(t,X) on denseExpiries × denseXStrikes; bilinear in t and X.
 * Shared by LV fast path and LSV simulators for apples-to-apples σ_LV reads.
 */
class BuehlerMcTimeGridSigmaLookup {
public:
    BuehlerMcTimeGridSigmaLookup(const QuantLib::ext::shared_ptr<QuantLib::LocalVolTermStructure>& lv,
                                 const BuehlerModel& buehler) {
        QL_REQUIRE(static_cast<bool>(lv), "BuehlerMcTimeGridSigmaLookup: empty local vol");
        const std::vector<QuantLib::Date>& expiryDates = buehler.denseExpiries();
        const std::vector<QuantLib::Real>& denseStrikes = buehler.denseXStrikes();
        QL_REQUIRE(expiryDates.size() >= 2, "BuehlerMcTimeGridSigmaLookup: denseExpiries too small");
        QL_REQUIRE(denseStrikes.size() >= 2, "BuehlerMcTimeGridSigmaLookup: denseXStrikes too small");

        const QuantLib::DayCounter& dc = buehler.dayCounter();
        const QuantLib::Date today = buehler.today();
        times_.resize(expiryDates.size());
        for (QuantLib::Size j = 0; j < expiryDates.size(); ++j)
            times_[j] = dc.yearFraction(today, expiryDates[j]);

        strikes_ = denseStrikes;
        kLo_ = strikes_.front();
        kHi_ = strikes_.back();
        QL_REQUIRE(kLo_ > 0.0 && kHi_ > kLo_, "BuehlerMcTimeGridSigmaLookup: invalid dense X range");

        const QuantLib::Size nT = times_.size();
        const QuantLib::Size nK = strikes_.size();
        vols_.resize(nT * nK);
        for (QuantLib::Size it = 0; it < nT; ++it) {
            const QuantLib::Time t = times_[it];
            for (QuantLib::Size ik = 0; ik < nK; ++ik)
                vols_[it * nK + ik] = lv->localVol(t, strikes_[ik], true);
        }

        // O(1) X indexing when the dense grid is (numerically) uniform; the slice
        // fix-up loops keep the bracket exact, so this only needs to be approximate.
        const QuantLib::Real step = (kHi_ - kLo_) / static_cast<QuantLib::Real>(nK - 1);
        bool uniform = step > 0.0;
        for (QuantLib::Size ik = 0; uniform && ik < nK; ++ik) {
            const QuantLib::Real expected = kLo_ + static_cast<QuantLib::Real>(ik) * step;
            if (std::fabs(strikes_[ik] - expected) > 1.0e-9 * std::max(1.0, std::fabs(expected)))
                uniform = false;
        }
        invStep_ = uniform ? 1.0 / step : 0.0;
    }

    /** @brief Resolve the time bracket once for a whole evolution step; then use @c atX per path. */
    BuehlerMcSigmaTimeSlice sliceAtTime(const QuantLib::Time t) const {
        const QuantLib::Time tClamped =
            std::min(std::max(t, times_.front()), times_.back());
        const QuantLib::Size nT = times_.size();
        const QuantLib::Size nK = strikes_.size();

        const auto tIt = std::lower_bound(times_.begin(), times_.end(), tClamped);
        QuantLib::Size i1 = static_cast<QuantLib::Size>(tIt - times_.begin());
        if (i1 >= nT)
            i1 = nT - 1;
        const QuantLib::Size i0 = (i1 == 0) ? 0 : i1 - 1;

        const QuantLib::Real t0 = times_[i0];
        const QuantLib::Real t1 = times_[i1];

        BuehlerMcSigmaTimeSlice slice;
        slice.strikes_ = &strikes_;
        slice.row0_ = vols_.data() + i0 * nK;
        slice.row1_ = vols_.data() + i1 * nK;
        slice.wt_ = (t1 > t0) ? (tClamped - t0) / (t1 - t0) : 0.0;
        slice.kLo_ = kLo_;
        slice.kHi_ = kHi_;
        slice.invStep_ = invStep_;
        return slice;
    }

    QuantLib::Real atTime(const QuantLib::Time t, const QuantLib::Real x) const {
        const QuantLib::Time tClamped =
            std::min(std::max(t, times_.front()), times_.back());
        const QuantLib::Real xClamped = std::min(std::max(x, kLo_), kHi_);

        const QuantLib::Size nT = times_.size();
        const QuantLib::Size nK = strikes_.size();

        const auto tIt = std::lower_bound(times_.begin(), times_.end(), tClamped);
        QuantLib::Size i1 = static_cast<QuantLib::Size>(tIt - times_.begin());
        if (i1 >= nT)
            i1 = nT - 1;
        QuantLib::Size i0 = (i1 == 0) ? 0 : i1 - 1;

        const auto kIt = std::lower_bound(strikes_.begin(), strikes_.end(), xClamped);
        QuantLib::Size j1 = static_cast<QuantLib::Size>(kIt - strikes_.begin());
        if (j1 >= nK)
            j1 = nK - 1;
        QuantLib::Size j0 = (j1 == 0) ? 0 : j1 - 1;

        const QuantLib::Real t0 = times_[i0];
        const QuantLib::Real t1 = times_[i1];
        const QuantLib::Real wt = (t1 > t0) ? (tClamped - t0) / (t1 - t0) : 0.0;

        const QuantLib::Real k0 = strikes_[j0];
        const QuantLib::Real k1 = strikes_[j1];
        const QuantLib::Real wk = (k1 > k0) ? (xClamped - k0) / (k1 - k0) : 0.0;

        const QuantLib::Real v00 = vols_[i0 * nK + j0];
        const QuantLib::Real v01 = vols_[i0 * nK + j1];
        const QuantLib::Real v10 = vols_[i1 * nK + j0];
        const QuantLib::Real v11 = vols_[i1 * nK + j1];

        const QuantLib::Real v0 = v00 + wk * (v01 - v00);
        const QuantLib::Real v1 = v10 + wk * (v11 - v10);
        return v0 + wt * (v1 - v0);
    }

private:
    QuantLib::Real kLo_ = 0.0;
    QuantLib::Real kHi_ = 0.0;
    QuantLib::Real invStep_ = 0.0;
    std::vector<QuantLib::Time> times_;
    std::vector<QuantLib::Real> strikes_;
    std::vector<QuantLib::Real> vols_;
};

#endif
