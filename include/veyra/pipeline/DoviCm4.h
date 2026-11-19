#pragma once
// CM v2.9 / v4.0 creative trim metadata for the per-pixel expansion.
//
// CB1's Adaptive HDR Mapping uses up to two "anchors" (target displays) to
// compute two things the plain L1 path cannot know:
//   * a retention ratio r(u) from each anchor's trim response, which shapes the
//     expansion table so authored creative intent survives, and
//   * two global colour coefficients plus a 65-point circular hue table that
//     restrain the expansion of saturated highlights (the chroma gate).
//
// This file carries the decoded anchors; the arithmetic lives in
// DoviEnhancedCurve.h so it can be unit tested without an RPU.
//
// Field names follow libdovi's DoviExtMetadataBlockLevel2/8 so a reader can
// compare them against `dovi_tool export` without a translation table.
//
// GPL-2.0-or-later arithmetic, see THIRD_PARTY_NOTICES.md (CB1 section).
// Custom addition for the Dolby Vision work; inert unless anchors are present.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace veyra::pipeline {

// One creative anchor: an L8 block (CM v4.0, preferred) or an L2 block (CM v2.9
// compatibility). L8 carries the per-secondary-colour vectors the hue table
// needs; an L2-only anchor leaves those zero, which is exactly CB1's
// "L2 compatibility only" behaviour.
struct DoviCm4Anchor {
    uint16_t targetMaxPq = 0;        // PQ (12-bit) of the anchor's target peak
    uint16_t trimSlope = 0;
    uint16_t trimOffset = 0;
    uint16_t trimPower = 0;
    uint16_t trimChromaWeight = 0;
    uint16_t trimSaturationGain = 0;
    uint16_t msWeight = 0;
    uint16_t targetMidContrast = 0;
    uint16_t clipTrim = 0;
    // L8 only, one entry per secondary colour (R, G, B, C, M, Y).
    std::array<uint8_t, 6> saturationVector{};
    std::array<uint8_t, 6> hueVector{};
    bool fromLevel8 = false;
};

inline constexpr int kDoviCm4MaxAnchors = 2;

// Everything the expansion needs to know about creative metadata for one frame.
// `count == 0` means the frame carried no usable anchor, and callers must then
// behave exactly as before (retention 1, no colour restraint).
struct DoviCm4Info {
    std::array<DoviCm4Anchor, kDoviCm4MaxAnchors> anchors{};
    uint8_t count = 0;
    uint8_t sourcePrimaryIndex = 0;  // L9; 0 = BT.2020
    uint8_t contentType = 0;         // L11
    bool hasContentType = false;

    bool valid() const { return count > 0; }
    void clear() { *this = DoviCm4Info{}; }
};

// PQ helpers kept local so the header has no dependency on the engine layer.
inline double cm4NitsToPq(double nits) {
    const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const double y = std::pow(std::clamp(nits, 0.0, 10000.0) / 10000.0, m1);
    return std::pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), m2);
}

inline double cm4PqToNits(double pq) {
    const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const double p = std::pow(std::clamp(pq, 0.0, 1.0), 1.0 / m2);
    return 10000.0 * std::pow(std::max(p - 3424.0 / 4096.0, 0.0) /
                                  std::max(2413.0 / 128.0 - 2392.0 / 128.0 * p, 1e-6),
                              1.0 / m1);
}

// A resolved pair of anchors for one display target.
//
// CB1: two bracketing anchors are weighted by PQ distance; with only endpoints
// available the nearest anchor is used and nothing is extrapolated.
struct DoviCm4Selection {
    const DoviCm4Anchor* nearAnchor = nullptr;
    const DoviCm4Anchor* farAnchor = nullptr;
    double nearWeight = 1.0;
    double farWeight = 0.0;
    bool valid() const { return nearAnchor != nullptr; }
};

inline DoviCm4Selection selectDoviCm4Anchors(const DoviCm4Info& info,
                                             uint16_t displayPeakPq) {
    DoviCm4Selection out;
    if (!info.valid() || displayPeakPq == 0) return out;
    const auto& a = info.anchors;
    const int n = int(std::min<int>(info.count, kDoviCm4MaxAnchors));

    // Find the highest anchor at or below the target and the lowest above it.
    const DoviCm4Anchor* below = nullptr;
    const DoviCm4Anchor* above = nullptr;
    for (int i = 0; i < n; ++i) {
        if (a[std::size_t(i)].targetMaxPq == 0) continue;
        if (a[std::size_t(i)].targetMaxPq <= displayPeakPq) {
            if (!below || a[std::size_t(i)].targetMaxPq > below->targetMaxPq) below = &a[std::size_t(i)];
        } else {
            if (!above || a[std::size_t(i)].targetMaxPq < above->targetMaxPq) above = &a[std::size_t(i)];
        }
    }
    if (below && above) {
        const double span = double(above->targetMaxPq) - double(below->targetMaxPq);
        const double t = span > 0.0 ? (double(displayPeakPq) - double(below->targetMaxPq)) / span : 0.0;
        out.nearAnchor = below;
        out.farAnchor = above;
        out.nearWeight = 1.0 - std::clamp(t, 0.0, 1.0);
        out.farWeight = std::clamp(t, 0.0, 1.0);
        return out;
    }
    // Only one side: CB1 uses the nearest anchor and does not extrapolate.
    const DoviCm4Anchor* nearest = nullptr;
    double best = 0.0;
    for (int i = 0; i < n; ++i) {
        if (a[std::size_t(i)].targetMaxPq == 0) continue;
        const double d = std::abs(double(a[std::size_t(i)].targetMaxPq) - double(displayPeakPq));
        if (!nearest || d < best) { nearest = &a[std::size_t(i)]; best = d; }
    }
    out.nearAnchor = nearest;
    out.nearWeight = nearest ? 1.0 : 0.0;
    return out;
}

// The two global colour coefficients and the 65-point hue table of CB1's
// `cb1_dv_enhanced_plan`. `hue[0..64]` is the circular lookup used by the chroma
// gate; entries 2..66 of CB1's `color[67]` are `hue[0..64]`.
//
// `chroma` and `saturation` are the weighted absolute log2 magnitudes of the
// decoded anchors' chroma weight and saturation gain; a secondary vector entry is
// the weighted absolute log2 gain plus its rotation in units of pi/12, blended
// with CB1's circular weights exp(4*(cos(hue-centre)-1)).
struct DoviCm4ColorTable {
    double chroma = 0.0;      // CB1 color[0]
    double saturation = 0.0;  // CB1 color[1]
    std::array<double, 65> hue{};
    bool valid = false;
};

inline void buildDoviCm4ColorTable(const DoviCm4Info& info,
                                   const DoviCm4Selection& selection,
                                   DoviCm4ColorTable& out) {
    out = DoviCm4ColorTable{};
    if (!selection.valid() || !info.valid()) return;

    // L8 vectors are 0..127 as an unsigned magnitude around the hue wheel; the
    // spec's hue centres put the six secondaries at 60 degree steps.
    constexpr double kStep = 6.283185307179586 / 6.0;
    double chroma = 0.0, saturation = 0.0;
    std::array<double, 6> gain{}, rotation{}, centre{};
    bool any = false;

    const auto accumulate = [&](const DoviCm4Anchor& a, double weight) {
        if (!(weight > 0.0)) return;
        any = true;
        // CB1 reads the magnitudes as log2 ratios around unity; the L8 fields are
        // unsigned deviations, so map 0..127 to a symmetric log2 magnitude.
        const double chromaW = std::fabs(std::log2(std::max(double(a.trimChromaWeight) / 128.0, 1e-3)));
        const double satW = std::fabs(std::log2(std::max(double(a.trimSaturationGain) / 128.0, 1e-3)));
        chroma += weight * chromaW;
        saturation += weight * satW;
        for (int j = 0; j < 6; ++j) {
            const double sv = double(a.saturationVector[std::size_t(j)]) / 128.0;
            const double hv = double(a.hueVector[std::size_t(j)]) / 128.0;
            gain[std::size_t(j)] += weight * std::fabs(std::log2(std::max(sv, 1e-3)));
            rotation[std::size_t(j)] += weight * std::fabs(hv) * 12.0;  // pi/12 units
            centre[std::size_t(j)] += weight * (double(j) * kStep - 3.141592653589793);
        }
    };
    accumulate(*selection.nearAnchor, selection.nearWeight);
    if (selection.farAnchor) accumulate(*selection.farAnchor, selection.farWeight);
    if (!any) return;

    out.chroma = chroma;
    out.saturation = saturation;
    for (int k = 0; k < 65; ++k) {
        const double hue = -3.141592653589793 + 6.283185307179586 * double(k) / 64.0;
        double sum = 0.0, value = 0.0;
        for (int j = 0; j < 6; ++j) {
            const double w = std::exp(4.0 * (std::cos(hue - centre[std::size_t(j)]) - 1.0));
            sum += w;
            value += w * (gain[std::size_t(j)] + rotation[std::size_t(j)]);
        }
        out.hue[std::size_t(k)] = sum > 0.0 ? value / sum : 0.0;
    }
    out.valid = true;
}

// Dolby's target display index -> peak, for L8 anchors whose L10 partner is
// absent. L2 always carries target_max_pq itself, so this table is only needed
// on the CM v4.0 path. Indices follow the authored target list (SDR, then the
// standard HDR targets); anything unknown resolves to 0 and the anchor is
// dropped rather than guessed.
inline uint16_t cm4KnownTargetPq(uint8_t index) {
    double nits = 0.0;
    switch (index) {
        case 0: nits = 100.0; break;     // SDR / 100 nit reference
        case 1: nits = 100.0; break;
        case 2: nits = 600.0; break;
        case 3: nits = 1000.0; break;
        case 4: nits = 2000.0; break;
        case 5: nits = 4000.0; break;
        case 6: nits = 10000.0; break;
        default: return 0;
    }
    return uint16_t(std::lround(cm4NitsToPq(nits) * 4095.0));
}

}  // namespace veyra::pipeline
