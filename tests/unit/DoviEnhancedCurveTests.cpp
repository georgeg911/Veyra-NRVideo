// Unit tests for 完整 B: the CB1-derived per-scene highlight expansion.
//
// The module is header-only and header-tested on purpose - the GPU path in
// shaders/PresentBlit.hlsl mirrors doviExpandRgb() line for line, so what is
// pinned here is the arithmetic every presented frame actually goes through.
//
// Covers: when a scene may expand at all (CB1's amount terms), the table's shape
// and units, the luminance addressing the shader uses, the soft knee that keeps a
// pixel inside the ceiling, and CB1's bounded temporal transition.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "veyra/pipeline/DoviEnhancedCurve.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

bool close(double a, double b, double tolerance = 1e-5) {
    const double scale = std::max({1.0, std::fabs(a), std::fabs(b)});
    return std::fabs(a - b) <= tolerance * scale;
}

veyra::pipeline::DoviExpansionPlan planFor(double master, double mid, double ceiling) {
    veyra::pipeline::DoviExpansionInput in;
    in.sourcePeakNits = master;
    in.sceneAvgNits = mid;
    in.displayPeakNits = ceiling;
    in.valid = true;
    return veyra::pipeline::buildDoviExpansion(in);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    using namespace veyra::pipeline;

    // ---- 1) a dark scene has no headroom worth using ----
    {
        const auto plan = planFor(100.0, 40.0, 1000.0);
        check(!plan.active, "dark scene: inactive");
        check(close(plan.amount, 0.0), "dark scene: amount is zero");
        check(plan.extra[0] == 0.0f && plan.extra[kDoviExtraEntries - 1] == 0.0f,
              "dark scene: table stays empty");
    }

    // ---- 2) a mid-bright scene expands, and the table is what CB1 describes ----
    {
        const auto plan = planFor(300.0, 60.0, 1000.0);
        check(plan.active, "300-nit scene under a 1000-nit ceiling: active");
        check(plan.amount > 0.4, "300-nit scene: amount is above 0.4");

        bool monotonic = true;
        for (int k = 1; k < kDoviExtraEntries; ++k)
            if (plan.extra[std::size_t(k)] < plan.extra[std::size_t(k - 1)]) monotonic = false;
        check(monotonic, "table is non-decreasing");

        check(close(plan.extra[0], 0.0), "table starts at zero lift");
        const double span = plan.displayPeakNits - plan.sourcePeakNits;
        check(close(double(plan.extra[kDoviExtraEntries - 1]), plan.amount * span, 1e-4),
              "table ends at amount * headroom (nits)");
        // Units: the last entry is a lift in nits, not a ratio. A 300-nit scene
        // must not gain more than the 700 nits it was given.
        check(double(plan.extra[kDoviExtraEntries - 1]) <= span + 1e-6,
              "lift never exceeds the headroom");
    }

    // ---- 3) the table is addressed by the source's own nits ----
    {
        const auto plan = planFor(300.0, 60.0, 1000.0);
        check(close(doviTableAt(plan, plan.sourcePeakNits), plan.extra[kDoviExtraEntries - 1], 1e-4),
              "lookup at the master peak reaches the last entry");
        check(close(doviTableAt(plan, 10.0), 0.0), "lookup at black is zero");
        check(close(doviTableAt(plan, 5.0), 0.0), "lookup below black is zero");
        check(close(doviTableAt(plan, plan.sourcePeakNits + 50.0), 0.0),
              "lookup above the master peak is refused, not clamped");
        const double middle = 10.0 + (plan.sourcePeakNits - 10.0) * 0.5;
        check(doviTableAt(plan, middle) > 0.0 && doviTableAt(plan, middle) < plan.extra[kDoviExtraEntries - 1],
              "half-way luminance lands between the ends");
    }

    // ---- 4) no headroom means no expansion ----
    {
        check(!planFor(1000.0, 200.0, 1000.0).active, "source at the ceiling: inactive");
        check(!planFor(1200.0, 200.0, 1000.0).active, "source above the ceiling: inactive");
    }

    // ---- 5) an implausible input is refused ----
    {
        DoviExpansionInput in;
        in.sourcePeakNits = 300.0;
        in.sceneAvgNits = 60.0;
        in.displayPeakNits = 1000.0;
        in.valid = false;
        check(!buildDoviExpansion(in).active, "invalid input: inactive");
        in.valid = true;
        in.sourcePeakNits = 0.0;
        check(!buildDoviExpansion(in).active, "zero master: inactive");
        in.sourcePeakNits = 20000.0;
        check(!buildDoviExpansion(in).active, "absurd master: inactive");
        in.sourcePeakNits = 300.0;
        in.displayPeakNits = 0.0;
        check(!buildDoviExpansion(in).active, "zero ceiling: inactive");
    }

    // ---- 6) per pixel: highlights lift, the ceiling is never crossed ----
    {
        const auto plan = planFor(300.0, 60.0, 1000.0);
        const double in[3] = {300.0, 300.0, 300.0};
        double out[3];
        doviExpandRgb(plan, in, out);
        check(out[0] > in[0] + 1e-6, "300-nit grey is raised");
        check(out[0] <= plan.displayPeakNits + 1e-6, "raised grey stays inside the ceiling");

        // A dark pixel keeps its value: the expansion is a highlight tool.
        const double dark[3] = {8.0, 8.0, 8.0};
        doviExpandRgb(plan, dark, out);
        check(out[0] == dark[0], "pixel at black is untouched");

        // The lift is applied to the luminance, not per channel: a saturated pixel
        // keeps its hue, so the three channels scale by the same factor.
        const double tinted[3] = {200.0, 120.0, 40.0};
        doviExpandRgb(plan, tinted, out);
        const double ratio = out[0] / tinted[0];
        check(close(out[1] / tinted[1], ratio, 1e-6) && close(out[2] / tinted[2], ratio, 1e-6),
              "expansion preserves hue (one luminance gain)");
    }

    // ---- 7) an inactive plan is the identity ----
    {
        DoviExpansionPlan none;
        const double in[3] = {500.0, 500.0, 500.0};
        double out[3];
        doviExpandRgb(none, in, out);
        check(out[0] == in[0] && out[1] == in[1] && out[2] == in[2], "inactive plan is the identity");
    }

    // ---- 8) CB1's bounded transition: a cut cannot flash the table ----
    {
        const auto from = planFor(300.0, 60.0, 1000.0);
        const auto to = planFor(120.0, 30.0, 1600.0);
        DoviExpansionPlan eased = to;
        transitionDoviExpansion(eased, from, 1.0 / 240.0, false);
        double change = 0.0;
        for (int k = 0; k < kDoviExtraEntries; ++k)
            change = std::max(change, std::fabs(double(eased.extra[std::size_t(k)]) -
                                                double(from.extra[std::size_t(k)])));
        // budget = 0.5 * dt * max(|target - master|, 0.12*(master-10)); the destination
        // plan's own numbers are the larger ones here.
        const double target = std::max(eased.displayPeakNits, eased.sourcePeakNits);
        const double budget = 0.5 * (1.0 / 240.0) *
                              std::max(target - eased.sourcePeakNits, 0.12 * (eased.sourcePeakNits - 10.0));
        check(change <= budget + 1e-4, "one frame of a cut moves no further than the budget");
        check(change > 0.0, "the transition still moves the table");
    }

    // ---- 9) reset and a first frame take the new table as it is ----
    {
        const auto from = planFor(300.0, 60.0, 1000.0);
        const auto to = planFor(120.0, 30.0, 1600.0);
        DoviExpansionPlan direct = to;
        transitionDoviExpansion(direct, from, 1.0 / 240.0, true);
        check(direct.extra[kDoviExtraEntries - 1] == to.extra[kDoviExtraEntries - 1],
              "reset adopts the new table directly");
    }

    // ---- 10) a whole second of the same change converges ----
    {
        const auto from = planFor(300.0, 60.0, 1000.0);
        const auto to = planFor(120.0, 30.0, 1600.0);
        // `previous` is what the last frame actually applied, exactly as the
        // presenter keeps it; easing a fixed reference would just stop early.
        DoviExpansionPlan previous = from;
        for (int frame = 0; frame < 240; ++frame) {
            DoviExpansionPlan next = to;
            transitionDoviExpansion(next, previous, 1.0 / 240.0, false);
            previous = next;
        }
        check(close(previous.extra[kDoviExtraEntries - 1], to.extra[kDoviExtraEntries - 1], 1e-3),
              "one second of easing reaches the new table");
    }

    // ---- 11) colour restraint: neutral anchors do nothing, trimmed ones do ----
    {
        DoviCm4Info cm4;
        cm4.count = 1;
        auto& anchor = cm4.anchors[0];
        anchor.targetMaxPq = uint16_t(std::lround(cm4NitsToPq(1000.0) * 4095.0));

        // A neutral anchor (all trim words at their centre) must leave the lift
        // alone even though the table is "present": that is the common CM v2.9
        // case where L2 exists but carries no creative trim.
        anchor.trimChromaWeight = 2048;
        anchor.trimSaturationGain = 2048;
        for (int j = 0; j < 6; ++j) { anchor.saturationVector[std::size_t(j)] = 128; anchor.hueVector[std::size_t(j)] = 128; }
        anchor.fromLevel8 = true;
        {
            DoviExpansionInput in;
            in.sourcePeakNits = 300.0; in.sceneAvgNits = 60.0; in.displayPeakNits = 1000.0; in.valid = true;
            const auto plan = buildDoviExpansion(in, &cm4);
            check(plan.active && plan.colorValid, "neutral anchor: the table is carried");
            const double rgb[3] = {400.0, 200.0, 60.0};
            check(close(doviColorRestraint(plan, rgb), 1.0, 1e-9),
                  "neutral anchor: restraint is exactly one");
        }

        // A saturated, trimmed anchor must restrain the lift, and never below
        // CB1's 0.65 floor.
        anchor.trimChromaWeight = 3200;
        anchor.trimSaturationGain = 3200;
        for (int j = 0; j < 6; ++j) { anchor.saturationVector[std::size_t(j)] = 220; anchor.hueVector[std::size_t(j)] = 240; }
        {
            DoviExpansionInput in;
            in.sourcePeakNits = 300.0; in.sceneAvgNits = 60.0; in.displayPeakNits = 1000.0; in.valid = true;
            const auto plan = buildDoviExpansion(in, &cm4);
            check(plan.active && plan.colorValid, "trimmed anchor: the table is carried");
            const double tinted[3] = {400.0, 120.0, 40.0};
            const double restraint = doviColorRestraint(plan, tinted);
            check(restraint < 1.0, "trimmed anchor: a saturated pixel is restrained");
            check(restraint >= 0.65 - 1e-9, "restraint never drops below the 0.65 floor");
            // Neutral grey bypasses the transform by contract.
            const double grey[3] = {300.0, 300.0, 300.0};
            check(close(doviColorRestraint(plan, grey), 1.0, 1e-9),
                  "neutral grey bypasses the colour restraint");
            // And the restrained pixel really receives less lift than the grey one.
            double tintedOut[3], greyOut[3];
            doviExpandRgb(plan, tinted, tintedOut);
            doviExpandRgb(plan, grey, greyOut);
            const double tintedGain = tintedOut[0] / tinted[0];
            const double greyGain = greyOut[0] / grey[0];
            check(tintedGain < greyGain, "a saturated pixel is lifted less than a neutral one");
        }
    }

    // ---- 12) no anchors: the restraint is the identity ----
    {
        DoviExpansionInput in;
        in.sourcePeakNits = 300.0; in.sceneAvgNits = 60.0; in.displayPeakNits = 1000.0; in.valid = true;
        const auto plan = buildDoviExpansion(in, nullptr);
        check(plan.active && !plan.colorValid, "no anchors: expansion without a colour table");
        const double rgb[3] = {400.0, 120.0, 40.0};
        check(close(doviColorRestraint(plan, rgb), 1.0, 1e-9), "no anchors: restraint is one");
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
