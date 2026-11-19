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

#include "veyra/pipeline/DoviCm4.h"
#include "veyra/pipeline/DoviCm4Trim.h"

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
    // Custom (完整 C): CB1's colour restraint, from the CM anchors. False for a
    // stream that carries L1 only, which leaves the expansion exactly as it was
    // before CM4 existed. `hue` is the 65-point circular table (k = 0..64 over
    // [-pi, pi]); `chroma`/`saturation` are the two global coefficients.
    std::array<float, 65> hue{};
    float chroma = 0.0f;
    float saturation = 0.0f;
    bool colorValid = false;
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
inline DoviExpansionPlan buildDoviExpansion(const DoviExpansionInput& in,
                                            const DoviCm4Info* cm4 = nullptr) {
    DoviExpansionPlan plan;
    if (!in.valid || !std::isfinite(in.sourcePeakNits) || !(in.sourcePeakNits > 10.0) ||
        in.sourcePeakNits > 10000.0 || !std::isfinite(in.displayPeakNits) ||
        !(in.displayPeakNits > 10.0) || in.displayPeakNits > 10000.0) {
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

    // Custom (完整 C): CB1's retention knots weight the shape, so the authored trim
    // response survives instead of being flattened by a scene-only ramp. Without
    // usable anchors every knot is 1.0 and the shape is exactly what it was before.
    std::array<float, kDoviExtraEntries> retention{};
    bool haveRetention = false;
    DoviCm4Selection selection;
    if (cm4 != nullptr && cm4->valid()) {
        const uint16_t displayPq = uint16_t(std::lround(cm4NitsToPq(in.displayPeakNits) * 4095.0));
        selection = selectDoviCm4Anchors(*cm4, displayPq);
        if (selection.valid())
            haveRetention = buildDoviRetention(*cm4, selection, in.sourcePeakNits, retention);
    }

    double accumulated = 0.0;
    double shape[kDoviExtraEntries]{};
    for (int k = 1; k < kDoviExtraEntries; ++k) {
        const double u = k / double(kDoviExtraEntries - 1);
        const double u0 = (k - 1) / double(kDoviExtraEntries - 1);
        // The contract's C[k] uses the average of the two neighbouring knots.
        const double weight = haveRetention
            ? 0.5 * (double(retention[std::size_t(k - 1)]) + double(retention[std::size_t(k)]))
            : 1.0;
        accumulated += (std::pow(u, exponent) - std::pow(u0, exponent)) * weight;
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
    // Custom (完整 C): colour restraint. The anchors are resolved against the
    // display peak the expansion was sized for, exactly as CB1 resolves them
    // against the configured target.
    // CB1 rejects the whole frame when any anchor is unusable; keep the weaker rule
    // here (the expansion still runs) but do not engage the colour gate from a
    // selection whose retention could not be built.
    if (active && selection.valid() && haveRetention) {
        DoviCm4ColorTable table;
        buildDoviCm4ColorTable(*cm4, selection, table);
        if (table.valid) {
            plan.colorValid = true;
            plan.chroma = float(table.chroma);
            plan.saturation = float(table.saturation);
            for (int k = 0; k < 65; ++k) plan.hue[std::size_t(k)] = float(table.hue[std::size_t(k)]);
        }
    }
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
    double change = 0.0, maximum = 0.0;
    for (int k = 0; k < kDoviExtraEntries; ++k) {
        const auto idx = std::size_t(k);
        change = std::max(change,
                          std::fabs(double(plan.extra[idx]) - double(previous.extra[idx])));
        maximum = std::max(maximum,
                           std::max(double(plan.extra[idx]), double(previous.extra[idx])));
    }
    // Custom (完整 C): CB1 also charges the colour table's movement to the same
    // budget (change += .5*(|dcolor0|+|dcolor1|+sector)*maximum), otherwise a cut
    // could switch the hue layout at full speed while the lift was throttled.
    if (plan.colorValid) {
        double sector = 0.0;
        for (int k = 0; k < 65; ++k) {
            const double prev = previous.colorValid ? double(previous.hue[std::size_t(k)]) : 0.0;
            sector = std::max(sector, std::fabs(double(plan.hue[std::size_t(k)]) - prev));
        }
        const double prevChroma = previous.colorValid ? double(previous.chroma) : 0.0;
        const double prevSaturation = previous.colorValid ? double(previous.saturation) : 0.0;
        change += 0.5 * (std::fabs(double(plan.chroma) - prevChroma) +
                         std::fabs(double(plan.saturation) - prevSaturation) + sector) * maximum;
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
    // Custom (完整 C): ease the colour table by the same alpha. When the previous
    // plan carried no table (the anchors just appeared) the reference is zero, not
    // the identity, so the restraint ramps in instead of switching on at full
    // strength for one frame.
    if (plan.colorValid) {
        for (int k = 0; k < 65; ++k) {
            const auto idx = std::size_t(k);
            const double prev = previous.colorValid ? double(previous.hue[idx]) : 0.0;
            plan.hue[idx] = float(prev + (double(plan.hue[idx]) - prev) * alpha);
        }
        const double prevChroma = previous.colorValid ? double(previous.chroma) : 0.0;
        const double prevSaturation = previous.colorValid ? double(previous.saturation) : 0.0;
        plan.chroma = float(prevChroma + (double(plan.chroma) - prevChroma) * alpha);
        plan.saturation = float(prevSaturation + (double(plan.saturation) - prevSaturation) * alpha);
        plan.colorValid = true;
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
    // CB1's lookup() clamps the index rather than refusing the sample, and so does
    // this. Refusing was wrong twice over: `sourcePeakNits` is the *smoothed* L1
    // peak, so on a brightening shot a pixel just above it lost its entire lift
    // while its neighbour kept it (a 0.2% input step became a 2x output step, which
    // reads as a broken highlight edge), and the contract's guard is per frame, not
    // per pixel. Above the peak the entry saturates, and because the lift is applied
    // as a ratio of the pixel's own luminance it self-limits instead of blowing up.
    const double u = std::clamp((nits - 10.0) / (plan.sourcePeakNits - 10.0), 0.0, 1.0) *
                     double(kDoviExtraEntries - 1);
    const int index = std::min(int(u), kDoviExtraEntries - 2);
    const double fraction = u - double(index);
    return double(plan.extra[std::size_t(index)]) * (1.0 - fraction) +
           double(plan.extra[std::size_t(index + 1)]) * fraction;
}

// CB1's chroma gate: the lift is one luminance gain across all three channels, so
// a saturated highlight would carry its colour past what the display can show.
// This returns the fraction of the lift that survives, bounded to [0.65, 1] as
// CB1 does. Neutral greys bypass the transform entirely.
inline double doviColorRestraint(const DoviExpansionPlan& plan, const double rgb[3]) {
    if (!plan.colorValid || !(plan.sourcePeakNits > 10.0)) return 1.0;
    if (rgb[0] == rgb[1] && rgb[1] == rgb[2]) return 1.0;
    // BT.2020 RGB -> LMS (the standard IPT matrix libplacebo uses) -> PQ, then
    // IPT's intensity/opponent channels.
    static constexpr double kRgb2Lms[3][3] = {
        {0.4122214708, 0.5363325363, 0.0514459929},
        {0.2119034982, 0.6806995451, 0.1073969566},
        {0.0883024619, 0.2817188376, 0.6299787005},
    };
    double v[3];
    for (int j = 0; j < 3; ++j) {
        const double lms = kRgb2Lms[j][0] * rgb[0] + kRgb2Lms[j][1] * rgb[1] + kRgb2Lms[j][2] * rgb[2];
        v[j] = cm4NitsToPq(std::max(lms, 0.0));
    }
    const double intensity = 0.4 * v[0] + 0.4 * v[1] + 0.2 * v[2];
    if (!(intensity > 1e-9)) return 1.0;
    const double ct = 4.455 * v[0] - 4.851 * v[1] + 0.396 * v[2];
    const double cp = 0.8056 * v[0] + 0.3572 * v[1] - 1.1628 * v[2];
    const double gate = detail::smoothstepRange(0.0, 0.15, std::hypot(ct, cp) / intensity);
    const double z = std::clamp(intensity / cm4NitsToPq(plan.sourcePeakNits), 0.0, 1.0);
    // 65-point circular lookup: the table spans [-pi, pi] in 64 steps.
    const double pos = (std::atan2(cp, ct) + 3.141592653589793) / 6.283185307179586 * 64.0;
    const int index = std::min(int(pos), 63);
    const double fraction = std::clamp(pos - double(index), 0.0, 1.0);
    const double hue = double(plan.hue[std::size_t(index)]) * (1.0 - fraction) +
                       double(plan.hue[std::size_t(index + 1)]) * fraction;
    const double severity = double(plan.chroma) * std::fabs(2.0 * z - 1.0) +
                            double(plan.saturation) + hue;
    return std::max(0.65, 1.0 / (1.0 + 0.5 * gate * severity));
}

// CB1's cb1_dv_enhanced_pixel: lift the BT.2020 luminance by the table's entry,
// convert that lift to
// a ratio of the pixel's own luminance, then bound the ratio with a soft knee so
// the result approaches `plan.displayPeakNits` instead of overshooting it.
inline void doviExpandRgb(const DoviExpansionPlan& plan, const double in[3], double out[3]) {
    out[0] = in[0]; out[1] = in[1]; out[2] = in[2];
    if (!plan.active) return;
    // BT.2020 luminance weights, identical to the shader's.
    const double y = 0.2627 * in[0] + 0.6780 * in[1] + 0.0593 * in[2];
    if (!(y > 10.0)) return;
    double extra = doviTableAt(plan, y);
    if (!(extra > 0.0)) return;
    // Custom (完整 C): the colour gate applies before the soft knee, exactly where
    // CB1 applies it.
    extra *= doviColorRestraint(plan, in);
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