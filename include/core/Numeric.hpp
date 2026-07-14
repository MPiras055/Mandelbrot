#pragma once
#include <complex>
#include <boost/multiprecision/cpp_bin_float.hpp>

/**
 * @file Numeric.hpp
 * @brief Single source of truth for the renderer's numeric types.
 *
 * @details Previously the high-precision float alias (BigFloat) was declared
 * verbatim in four different headers, and the double-precision complex alias in
 * two. Consolidating them here means the backing precision can be changed in one
 * place. The existing scoped aliases (e.g. engine::BigFloat,
 * RenderJob::BigFloat, PerturbationJob::ComplexDouble) are kept as thin
 * re-exports so no call sites change.
 */
namespace core {

    // 50 decimal digits of mantissa; used for the camera center and reference
    // orbit generation where double precision is exhausted at deep zoom.
    using BigFloat = boost::multiprecision::cpp_bin_float_50;

    // Per-pixel delta arithmetic and cached reference orbits stay in double.
    using ComplexDouble = std::complex<double>;

    // Escape radius squared for the z = z^2 + c iteration.
    inline constexpr double ESCAPE_R2 = 4.0;

} // namespace core
