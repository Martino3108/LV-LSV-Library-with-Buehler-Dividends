/**
 * @file linear_time_cubic_strike_interpolation.h
 * @brief BlackVarianceSurface factory: linear in T on w=σ²T, monotonic cubic in strike.
 */

#ifndef LINEAR_TIME_CUBIC_STRIKE_INTERPOLATION_H
#define LINEAR_TIME_CUBIC_STRIKE_INTERPOLATION_H

#include <ql/math/interpolations/cubicinterpolation.hpp>
#include <ql/math/interpolations/interpolation2d.hpp>
#include <vector>

namespace QuantLib {

namespace detail {

template <class I1, class I2, class M>
class LinearTimeCubicStrikeImpl : public Interpolation2D::templateImpl<I1, I2, M> {
  public:
    LinearTimeCubicStrikeImpl(const I1& xBegin, const I1& xEnd,
                              const I2& yBegin, const I2& yEnd,
                              const M& zData)
    : Interpolation2D::templateImpl<I1, I2, M>(xBegin, xEnd, yBegin, yEnd, zData) {
        LinearTimeCubicStrikeImpl::calculate();
    }

    void calculate() override {
        const Size nT = static_cast<Size>(this->xEnd_ - this->xBegin_);
        splines_.clear();
        splines_.reserve(nT);
        for (Size j = 0; j < nT; ++j) {
            splines_.push_back(MonotonicCubicNaturalSpline(
                this->yBegin_, this->yEnd_, this->zData_.column_begin(j)));
        }
    }

    Real value(Real x, Real y) const override {
        const Size i = this->locateX(x);
        const Real x1 = this->xBegin_[i];
        const Real x2 = this->xBegin_[i + 1];
        const Real w1 = splines_[i](y, true);
        const Real w2 = splines_[i + 1](y, true);
        if (x2 <= x1)
            return w1;
        const Real lam = (x - x1) / (x2 - x1);
        return (1.0 - lam) * w1 + lam * w2;
    }

  private:
    std::vector<MonotonicCubicNaturalSpline> splines_;
};

} // namespace detail

//! Interpolation2D: monotonic cubic in strike (y), linear blend in time (x) of total variance.
class LinearTimeCubicStrikeInterpolation : public Interpolation2D {
  public:
    template <class I1, class I2, class M>
    LinearTimeCubicStrikeInterpolation(const I1& xBegin, const I1& xEnd,
                                       const I2& yBegin, const I2& yEnd,
                                       const M& zData) {
        impl_ = ext::shared_ptr<Interpolation2D::Impl>(
            new detail::LinearTimeCubicStrikeImpl<I1, I2, M>(
                xBegin, xEnd, yBegin, yEnd, zData));
    }
};

//! Factory for @c BlackVarianceSurface::setInterpolation.
class LinearTimeCubicStrike {
  public:
    template <class I1, class I2, class M>
    Interpolation2D interpolate(const I1& xBegin, const I1& xEnd,
                                const I2& yBegin, const I2& yEnd,
                                const M& z) const {
        return LinearTimeCubicStrikeInterpolation(
            xBegin, xEnd, yBegin, yEnd, z);
    }
};

} // namespace QuantLib

#endif
