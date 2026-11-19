#pragma once
// Adaptive highlight expansion for Dolby Vision scenes.
//
// Derived from CB1's Adaptive HDR Mapping (CroqueMr/libreelec-CB1,
// GPL-2.0-or-later): the per-scene weight and the 257-point curve shape follow
// CB1/docs/ALGORITHMS.md, and the bounded temporal transition follows
// cb1_dv_enhanced_transition. No CB1 source file was copied; only the published
// arithmetic is reproduced, so the file keeps Veyra's own GPL-3.0 license.
//
// The static HDR curve owns the display mapping. This module only decides how
// much extra headroom a scene may use and hands the GPU a 257-entry table; the
// existing curve family is untouched when the table is all zero.
//
// Custom addition for the Dolby Vision work; inert unless the caller opts in.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace veyra::pipeline {

// Number of table entries the shader samples (last index = 256).
inline constexpr int kDoviExtraEntries = 257;

struct DoviExpansionInput {
    double sourcePeakNits = 0.0;   // RPU L1 max, PQ-decoded to nits
    double sceneAvgNits = 0.0;     // RPU L1 average, PQ-decoded to nits
    double displayPeakNits = 0.0;  // target display peak
    bool valid = false;
};

struct DoviExpansionPlan {
    std::array<float, kDoviExtraEntries> extra{};  // nits of lift per entry
    double amount = 0.0;      // scene weight, 0..1
    double sourcePeakNits = 0.0;
    double displayPeakNits = 0.0;
    bool active = false;
};

namespace detail {

inline double smoothstep01(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

inline double smoothstepRange(double lo, double hi, double x) {
    if (!(hi > lo)) return x >= hi ? 1.0 : 0.0;
    return smoothstep01((x - lo) / (hi - lo));
}

}  // namespace detail

// Build the per-scene table. `reference` is the scene's own highlight level: it
// caps how far the expansion may reach, so a dark scene is not pushed to the
// display peak. `amount` combines the highlight level (CB1's smooth(100,400))
// with the midtone share (1 - 0.5*smooth(0.04,0.30)).
inline DoviExpansionPlan buildDoviExpansion(const DoviExpansionInput& in) {
    DoviExpansionPlan plan;
    if (!in.valid || !(in.sourcePeakNits > 10.0) || in.sourcePeakNits > 10000.0 ||
        !(in.displayPeakNits > 10.0)) {
        return plan;
    }
    plan.sourcePeakNits = in.sourcePeakNits;
    plan.displayPeakNits = in.displayPeakNits;

    const double high = std::min(in.sourcePeakNits, in.displayPeakNits);
    const double midtoneShare =
        in.sourcePeakNits > 1e-9 ? in.sceneAvgNits / in.sourcePeakNits : 1.0;
    const double amount =
        detail::smoothstepRange(100.0, 400.0, in.sourcePeakNits) *
        (1.0 - 0.5 * detail::smoothstepRange(0.04, 0.30, midtoneShare));
    plan.amount = amount;
    if (!(amount > 0.0)) return plan;

    // Extend the scene's own highlight toward the display peak, never past it.
    const double ceil = in.displayPeakNits;
    const double base = std::max(in.sourcePeakNits, high);
    const double span = std::max(0.0, ceil - base);
    if (!(span > 0.0)) return plan;

    // Shape: a smooth ramp that reaches full lift at the scene's highlight and
    // keeps a small cushion below it so the roll-off does not step.
    const double exponent = 2.0 + 2.0 * detail::smoothstepRange(0.04, 0.30, midtoneShare);
    double accumulated = 0.0;
    double shape[kDoviExtraEntries]{};
    for (int k = 1; k < kDoviExtraEntries; ++k) {
        const double u = k / double(kDoviExtraEntries - 1);
        const double u0 = (k - 1) / double(kDoviExtraEntries - 1);
        accumulated += (std::pow(u, exponent) - std::pow(u0, exponent));
        shape[k] = accumulated;
    }
    const double normalization = std::max(shape[kDoviExtraEntries - 1], 1e-9);
    const double lift = amount * span;
    bool active = false;
    for (int k = 0; k < kDoviExtraEntries; ++k) {
        const double value = lift * shape[k] / normalization;
        plan.extra[std::size_t(k)] = float(value);
        active = active || value > 0.0;
    }
    plan.active = active;
    return plan;
}

// Bounded temporal transition: never move faster than ~half a display peak per
// second (CB1's budget), so a cut cannot flash the expansion.
inline void transitionDoviExpansion(DoviExpansionPlan& plan,
                                    const DoviExpansionPlan& previous,
                                    double dtSeconds,
                                    bool reset) {
    if (reset || !(dtSeconds > 0.0) || dtSeconds > 1.0) return;
    if (!plan.active || !previous.active) return;
    double change = 0.0;
    for (int k = 0; k < kDoviExtraEntries; ++k) {
        change = std::max(change,
                          std::fabs(double(plan.extra[std::size_t(k)]) -
                                    double(previous.extra[std::size_t(k)])));
    }
    if (!(change > 1e-9)) return;
    const double target = std::max(plan.displayPeakNits, plan.sourcePeakNits);
    const double budget = 0.5 * dtSeconds * std::max(target - plan.sourcePeakNits,
                                                    0.12 * plan.sourcePeakNits);
    const double alpha = std::clamp(budget / change, 0.0, 1.0);
    for (int k = 0; k < kDoviExtraEntries; ++k) {
        const auto idx = std::size_t(k);
        plan.extra[idx] = float(previous.extra[idx] +
                                (plan.extra[idx] - previous.extra[idx]) * alpha);
    }
}

// ---------------------------------------------------------------------------
// Per-pixel application (custom).
//
// This is the reference the GPU path is checked against: shaders/PresentBlit.hlsl
// DoviExpand performs exactly these steps on absolute nits, so a unit test here
// pins the arithmetic a frame actually goes through. Keep the two in step - the
// soft knee in particular only means anything if both sides compute it the same.
// ---------------------------------------------------------------------------

// Linear interpolation of the table at a luminance, using the same 0..256 scale
// the shader indexes with: the table is addressed by the source's own nits.
inline double doviTableAt(const DoviExpansionPlan& plan, double nits) {
    if (!(nits > 10.0) || !(plan.sourcePeakNits > 10.0)) return 0.0;
    // CB1 refuses a pixel above the master peak instead of clamping its index: the
    // metadata then disagrees with the picture, and the brightest content must not
    // collect the largest lift. The 0.1% tolerance absorbs the BT.2020 weight
    // rounding at exactly the master, which would otherwise reject the top step.
    if (nits > plan.sourcePeakNits * 1.001) return 0.0;
    const double u = std::clamp((nits - 10.0) / (plan.sourcePeakNits - 10.0), 0.0, 1.0) *
                     double(kDoviExtraEntries - 1);
    const int index = std::min(int(u), kDoviExtraEntries - 2);
    const double fraction = u - double(index);
    return double(plan.extra[std::size_t(index)]) * (1.0 - fraction) +
           double(plan.extra[std::size_t(index + 1)]) * fraction;
}

// CB1's cb1_dv_enhanced_pixel without the CM4 hue retention (Veyra has no L2/L8
// anchors): lift the BT.2020 luminance by the table's entry, convert that lift to
// a ratio of the pixel's own luminance, then bound the ratio with a soft knee so
// the result approaches `plan.displayPeakNits` instead of overshooting it.
inline void doviExpandRgb(const DoviExpansionPlan& plan, const double in[3], double out[3]) {
    out[0] = in[0]; out[1] = in[1]; out[2] = in[2];
    if (!plan.active) return;
    // BT.2020 luminance weights, identical to the shader's.
    const double y = 0.2627 * in[0] + 0.6780 * in[1] + 0.0593 * in[2];
    if (!(y > 10.0)) return;
    const double extra = doviTableAt(plan, y);
    if (!(extra > 0.0)) return;
    const double peak = std::max({in[0], in[1], in[2]});
    const double headroom = std::max(0.0, plan.displayPeakNits / std::max(peak, 1e-3) - 1.0);
    const double knee = 0.9 * headroom, width = 0.1 * headroom;
    double delta = extra / y;
    if (delta > knee) {
        const double tail = delta - knee;
        delta = knee + (width + tail > 0.0 ? width * tail / (width + tail) : 0.0);
    }
    for (int j = 0; j < 3; ++j) out[j] = in[j] * (1.0 + delta);
}

}  // namespace veyra::pipeline