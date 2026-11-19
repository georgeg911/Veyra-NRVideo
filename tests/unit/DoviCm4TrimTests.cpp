// Unit tests for 完整 C 的 Dolby Vision trim 算子与 retention 表。
//
// 模块是纯头文件（include/veyra/pipeline/DoviCm4Trim.h），这里直接头文件测试，
// 不需要链接引擎：retention 表是 CPU 侧算出来交给下层的，先把它钉住，GPU/上层
// 才有参照。
//
// 覆盖：契约的 r(u) 公式（中性=1、slope 偏离、[0.5,1.15] 夹取、双锚点加权），
// CM4 的 midtone/clipping 控制只在 L8 锚点上生效，以及无效输入（master 越界、
// 锚点不可用、权重和不为 1）必须安全地返回 false 并把表填成中性。
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include "veyra/pipeline/DoviCm4Trim.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

bool close(double a, double b, double tolerance = 1e-6) {
    const double scale = std::max({1.0, std::fabs(a), std::fabs(b)});
    return std::fabs(a - b) <= tolerance * scale;
}

veyra::pipeline::DoviCm4Anchor anchorAt(double nits) {
    veyra::pipeline::DoviCm4Anchor anchor;
    anchor.targetMaxPq =
        uint16_t(std::lround(veyra::pipeline::cm4NitsToPq(nits) * 4095.0));
    return anchor;
}

// 所有 12-bit trim 字都放在中性 2048（= 1.0）。
void neutralise(veyra::pipeline::DoviCm4Anchor& anchor) {
    anchor.trimSlope = 2048;
    anchor.trimOffset = 2048;
    anchor.trimPower = 2048;
    anchor.trimChromaWeight = 2048;
    anchor.trimSaturationGain = 2048;
    anchor.msWeight = 2048;
    anchor.targetMidContrast = 2048;
    anchor.clipTrim = 2048;
}

bool finiteTable(const std::array<float, veyra::pipeline::kDoviRetentionEntries>& r) {
    for (float v : r)
        if (!std::isfinite(v)) return false;
    return true;
}

bool withinBounds(const std::array<float, veyra::pipeline::kDoviRetentionEntries>& r) {
    for (float v : r)
        if (v < 0.5f - 1e-6f || v > 1.15f + 1e-6f) return false;
    return true;
}

bool allNeutral(const std::array<float, veyra::pipeline::kDoviRetentionEntries>& r) {
    for (float v : r)
        if (!close(double(v), 1.0, 1e-6)) return false;
    return true;
}

// 单锚点、权重 1 的 selection。
veyra::pipeline::DoviCm4Selection singleAnchor(const veyra::pipeline::DoviCm4Anchor& anchor) {
    veyra::pipeline::DoviCm4Selection selection;
    selection.nearAnchor = &anchor;
    selection.nearWeight = 1.0;
    return selection;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    using namespace veyra::pipeline;

    // ---- 1) 中性锚点：retention 恒为 1，trim 算子是恒等 ----
    {
        DoviCm4Info info;
        info.count = 1;
        info.anchors[0] = anchorAt(1000.0);
        neutralise(info.anchors[0]);
        std::array<float, kDoviRetentionEntries> r;
        check(buildDoviRetention(info, singleAnchor(info.anchors[0]), 1000.0, r),
              "neutral anchor: retention builds");
        check(finiteTable(r), "neutral anchor: table is finite");
        check(allNeutral(r), "neutral anchor: every r[k] is 1.0");

        // master 和锚点峰值不同也一样：中性 trim 不依赖采样域。
        check(buildDoviRetention(info, singleAnchor(info.anchors[0]), 4000.0, r),
              "neutral anchor: other master still builds");
        check(allNeutral(r), "neutral anchor: other master leaves r at 1.0");

        // 算子本身必须是 nits 恒等（abs nits 域）。
        bool identity = true;
        for (double nits : {0.0, 10.0, 100.0, 1000.0, 4000.0}) {
            const double in[3] = {nits, nits, nits};
            double out[3];
            doviTrimApply(info.anchors[0], in, out);
            identity = identity && close(out[0], nits, 1e-9) && close(out[1], nits, 1e-9) &&
                       close(out[2], nits, 1e-9);
        }
        check(identity, "neutral anchor: doviTrimApply is the identity in nits");
    }

    // ---- 2) slope 偏离 2048：r 偏离 1，并被夹在 [0.5,1.15] ----
    {
        DoviCm4Info info;
        info.count = 1;
        info.anchors[0] = anchorAt(1000.0);
        neutralise(info.anchors[0]);
        info.anchors[0].trimSlope = 2013;  // 略低于中性：-1.7%
        std::array<float, kDoviRetentionEntries> r;
        check(buildDoviRetention(info, singleAnchor(info.anchors[0]), 1000.0, r),
              "slope below neutral: retention builds");
        check(finiteTable(r) && withinBounds(r), "slope below neutral: finite and clamped");
        check(r[0] < 0.98f && r[kDoviRetentionEntries - 1] < 0.95f,
              "slope below neutral: r drops below 1");
        bool nonIncreasing = true;
        for (int k = 1; k < kDoviRetentionEntries; ++k)
            if (double(r[std::size_t(k)]) > double(r[std::size_t(k - 1)]) + 1e-6) nonIncreasing = false;
        check(nonIncreasing, "slope below neutral: r is non-increasing over u");

        info.anchors[0].trimSlope = 2600;  // 大幅高于中性：必须撞上 1.15 的夹取
        check(buildDoviRetention(info, singleAnchor(info.anchors[0]), 1000.0, r),
              "slope above neutral: retention builds");
        bool allUpper = true;
        for (float v : r) allUpper = allUpper && close(double(v), 1.15, 1e-5);
        check(allUpper, "slope above neutral: r is pinned at the 1.15 clamp");

        info.anchors[0].trimSlope = 1200;  // 大幅低于中性：必须撞上 0.5 的夹取
        check(buildDoviRetention(info, singleAnchor(info.anchors[0]), 1000.0, r),
              "slope far below neutral: retention builds");
        bool allLower = true;
        for (float v : r) allLower = allLower && close(double(v), 0.5, 1e-6);
        check(allLower, "slope far below neutral: r is pinned at the 0.5 clamp");
    }

    // ---- 3) power 只产生有限的、单调的 guidance；极端字段不产生 NaN/Inf ----
    {
        DoviCm4Info info;
        info.count = 1;
        info.anchors[0] = anchorAt(1000.0);
        neutralise(info.anchors[0]);
        info.anchors[0].trimPower = 2300;  // 幂 > 1：暗部 r 变小，白点仍是 1
        std::array<float, kDoviRetentionEntries> r;
        check(buildDoviRetention(info, singleAnchor(info.anchors[0]), 1000.0, r),
              "power above neutral: retention builds");
        check(finiteTable(r) && withinBounds(r), "power above neutral: finite and clamped");
        check(r[0] < 0.7f && close(double(r[kDoviRetentionEntries - 1]), 1.0, 1e-4),
              "power above neutral: dark end drops, white stays at 1");
        bool nonDecreasing = true;
        for (int k = 1; k < kDoviRetentionEntries; ++k)
            if (r[std::size_t(k)] < r[std::size_t(k - 1)] - 1e-6f) nonDecreasing = false;
        check(nonDecreasing, "power above neutral: r is non-decreasing over u");

        // 所有字拉到最大值：无论算子内部怎么放大，表都必须有限且仍在夹取范围内。
        info.anchors[0] = anchorAt(1000.0);
        neutralise(info.anchors[0]);
        info.anchors[0].fromLevel8 = true;
        info.anchors[0].trimSlope = 4095;
        info.anchors[0].trimOffset = 4095;
        info.anchors[0].trimPower = 4095;
        info.anchors[0].targetMidContrast = 4095;
        info.anchors[0].clipTrim = 4095;
        check(buildDoviRetention(info, singleAnchor(info.anchors[0]), 1000.0, r),
              "extreme trim words: retention builds");
        check(finiteTable(r), "extreme trim words: table has no NaN/Inf");
        check(withinBounds(r), "extreme trim words: table stays inside [0.5,1.15]");
        const double in[3] = {0.0, 500.0, 10000.0};
        double out[3];
        doviTrimApply(info.anchors[0], in, out);
        check(std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]),
              "extreme trim words: operator output is finite");
        check(out[0] >= 0.0 && out[1] >= 0.0 && out[2] <= 10000.0,
              "extreme trim words: operator output stays in [0,10000]");
    }

    // ---- 4) 无效输入一律 false，且表退回中性 1.0 ----
    {
        DoviCm4Info info;
        info.count = 1;
        info.anchors[0] = anchorAt(1000.0);
        neutralise(info.anchors[0]);
        const auto selection = singleAnchor(info.anchors[0]);
        std::array<float, kDoviRetentionEntries> r;

        const double badMasters[] = {0.0, -1.0, 5.0, 10.0, -std::numeric_limits<double>::infinity(),
                                     10000.5, 20000.0, std::numeric_limits<double>::quiet_NaN(),
                                     std::numeric_limits<double>::infinity()};
        for (double master : badMasters) {
            const bool ok = buildDoviRetention(info, selection, master, r);
            check(!ok, "invalid master: retention refuses (master=" + std::to_string(master) + ")");
            check(allNeutral(r), "invalid master: table falls back to 1.0");
        }

        DoviCm4Info noAnchors;
        check(!buildDoviRetention(noAnchors, selection, 1000.0, r),
              "no anchors: retention refuses");
        check(allNeutral(r), "no anchors: table falls back to 1.0");

        DoviCm4Selection noSelection;
        check(!buildDoviRetention(info, noSelection, 1000.0, r),
              "empty selection: retention refuses");

        DoviCm4Anchor badTarget = info.anchors[0];
        badTarget.targetMaxPq = 0;
        DoviCm4Info badInfo;
        badInfo.count = 1;
        badInfo.anchors[0] = badTarget;
        check(!buildDoviRetention(badInfo, singleAnchor(badInfo.anchors[0]), 1000.0, r),
              "anchor without target peak: retention refuses");

        DoviCm4Selection halfWeight = selection;
        halfWeight.nearWeight = 0.5;  // 权重和不是 1
        check(!buildDoviRetention(info, halfWeight, 1000.0, r),
              "weights not summing to one: retention refuses");

        DoviCm4Selection nanWeight = selection;
        nanWeight.nearWeight = std::numeric_limits<double>::quiet_NaN();
        check(!buildDoviRetention(info, nanWeight, 1000.0, r),
              "non-finite weight: retention refuses");

        DoviCm4Info two;
        two.count = 2;
        two.anchors[0] = anchorAt(600.0);
        two.anchors[1] = anchorAt(1000.0);
        neutralise(two.anchors[0]);
        neutralise(two.anchors[1]);
        DoviCm4Selection badSum;
        badSum.nearAnchor = &two.anchors[0];
        badSum.farAnchor = &two.anchors[1];
        badSum.nearWeight = 0.5;
        badSum.farWeight = 0.2;
        check(!buildDoviRetention(two, badSum, 1000.0, r),
              "two anchors with weights not summing to one: retention refuses");

        DoviCm4Selection negative;
        negative.nearAnchor = &two.anchors[0];
        negative.farAnchor = &two.anchors[1];
        negative.nearWeight = 1.5;
        negative.farWeight = -0.5;
        check(!buildDoviRetention(two, negative, 1000.0, r),
              "negative anchor weight: retention refuses");
    }

    // ---- 5) CM4 的 midtone / clipping 控制只在 L8 锚点上生效 ----
    {
        DoviCm4Info info;
        info.count = 1;
        info.anchors[0] = anchorAt(1000.0);
        neutralise(info.anchors[0]);
        info.anchors[0].fromLevel8 = true;
        info.anchors[0].targetMidContrast = 2400;  // 提高中间调对比
        std::array<float, kDoviRetentionEntries> r;
        check(buildDoviRetention(info, singleAnchor(info.anchors[0]), 1000.0, r),
              "L8 mid contrast: retention builds");
        check(finiteTable(r) && withinBounds(r), "L8 mid contrast: finite and clamped");
        check(r[0] < 0.9f, "L8 mid contrast: shadows are pulled down");
        check(*std::max_element(r.begin(), r.end()) > 1.02f,
              "L8 mid contrast: upper midtones are pushed up");
        check(close(double(r[kDoviRetentionEntries - 1]), 1.0, 1e-4),
              "L8 mid contrast: white point is preserved");

        // 同样的字放在 L2 锚点上必须完全被忽略：CM2.9 没有这两个控制。
        DoviCm4Info l2 = info;
        l2.anchors[0].fromLevel8 = false;
        check(buildDoviRetention(l2, singleAnchor(l2.anchors[0]), 1000.0, r),
              "L2 anchor with L8-only words: retention builds");
        check(allNeutral(r), "L2 anchor: midtone/clipping words are ignored");

        // Highlight Clipping：只压高光，白点明显下降，暗部不动。
        info.anchors[0].targetMidContrast = 2048;
        info.anchors[0].clipTrim = 2800;
        check(buildDoviRetention(info, singleAnchor(info.anchors[0]), 1000.0, r),
              "L8 highlight clipping: retention builds");
        check(close(double(r[0]), 1.0, 1e-4), "L8 highlight clipping: shadows untouched");
        check(r[kDoviRetentionEntries - 1] < 0.6f,
              "L8 highlight clipping: white point is compressed");
        check(finiteTable(r) && withinBounds(r), "L8 highlight clipping: finite and clamped");

        // L8 块里没带 midtone/clipping 字段（libdovi 留 0）时等价于中性。
        DoviCm4Info absent;
        absent.count = 1;
        absent.anchors[0] = anchorAt(1000.0);
        neutralise(absent.anchors[0]);
        absent.anchors[0].fromLevel8 = true;
        absent.anchors[0].targetMidContrast = 0;
        absent.anchors[0].clipTrim = 0;
        check(buildDoviRetention(absent, singleAnchor(absent.anchors[0]), 1000.0, r) && allNeutral(r),
              "absent L8 midtone/clipping fields: neutral");
    }

    // ---- 6) 两个锚点按 PQ 距离权重混合：先算响应，再线性加权 ----
    {
        DoviCm4Info info;
        info.count = 2;
        info.anchors[0] = anchorAt(600.0);
        neutralise(info.anchors[0]);
        info.anchors[0].trimSlope = 2200;  // 激进
        info.anchors[1] = anchorAt(1000.0);
        neutralise(info.anchors[1]);
        info.anchors[1].trimSlope = 1800;  // 保守

        std::array<float, kDoviRetentionEntries> blended, low, high;
        DoviCm4Selection both;
        both.nearAnchor = &info.anchors[0];
        both.farAnchor = &info.anchors[1];
        both.nearWeight = 0.5;
        both.farWeight = 0.5;
        check(buildDoviRetention(info, both, 1000.0, blended),
              "two anchors: retention builds");
        check(buildDoviRetention(info, singleAnchor(info.anchors[0]), 1000.0, low),
              "two anchors: near response builds");
        check(buildDoviRetention(info, singleAnchor(info.anchors[1]), 1000.0, high),
              "two anchors: far response builds");
        double maxError = 0.0;
        for (int k = 0; k < kDoviRetentionEntries; ++k)
            maxError = std::max(maxError, std::fabs(double(blended[std::size_t(k)]) -
                                                    0.5 * (double(low[std::size_t(k)]) +
                                                           double(high[std::size_t(k)]))));
        check(maxError <= 1e-5, "two anchors: blended table is the weighted mean");
        check(finiteTable(blended) && withinBounds(blended),
              "two anchors: blended table is finite and clamped");
    }

    // ---- 7) 算子本身的防御行为 ----
    {
        DoviCm4Anchor noTarget;  // targetMaxPq == 0 -> 没有可用锚点
        const double in[3] = {123.0, 45.0, 6.0};
        double out[3];
        doviTrimApply(noTarget, in, out);
        check(out[0] == in[0] && out[1] == in[1] && out[2] == in[2],
              "operator without target peak: identity");

        DoviCm4Anchor dark = anchorAt(1000.0);
        neutralise(dark);
        dark.trimSlope = 1024;  // 0.5x：一切都该变暗
        const double midGrey[3] = {500.0, 500.0, 500.0};
        doviTrimApply(dark, midGrey, out);
        check(out[0] < 500.0, "operator slope 0.5: mid grey goes down");

        DoviCm4Anchor bright = anchorAt(1000.0);
        neutralise(bright);
        bright.trimSlope = 3072;  // 1.5x
        doviTrimApply(bright, midGrey, out);
        check(out[0] > 500.0 && out[0] <= 10000.0, "operator slope 1.5: mid grey goes up");

        const double nanIn[3] = {std::numeric_limits<double>::quiet_NaN(), 100.0, -5.0};
        doviTrimApply(bright, nanIn, out);
        check(std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]),
              "operator with a non-finite sample: output stays finite");
    }

    // ---- tuning: the assumed constants are reachable, defaults unchanged ----
    //
    // A4 (the mid-contrast pivot) and A5 (the clip knee shape) are not in the
    // contract, so they live in DoviCm4TrimTuning. Moving them must change the
    // curve (that is what makes a future A/B possible), and the defaults must be
    // exactly what the rest of this suite pins.
    {
        using namespace veyra::pipeline;
        DoviCm4Anchor anchor = anchorAt(1000.0);
        neutralise(anchor);
        anchor.fromLevel8 = true;
        anchor.targetMidContrast = 2400;   // non-neutral mid contrast
        anchor.clipTrim = 2800;            // non-neutral clip

        DoviCm4Info info;
        info.count = 1;
        info.anchors[0] = anchor;
        const DoviCm4Selection sel =
            selectDoviCm4Anchors(info, uint16_t(std::lround(cm4NitsToPq(1000.0) * 4095.0)));
        check(sel.valid(), "tuning: the anchor resolves");

        std::array<float, 257> rDefault{}, rExplicit{}, rPivot{}, rClip{};
        check(buildDoviRetention(info, sel, 442.8, rDefault), "tuning: default build succeeds");
        check(buildDoviRetention(info, sel, 442.8, rExplicit, DoviCm4TrimTuning{}),
              "tuning: explicit default build succeeds");
        check(rDefault == rExplicit, "tuning: explicit defaults match the implicit ones");

        DoviCm4TrimTuning pivotTuning;
        pivotTuning.midContrastPivotFraction = 0.5;
        check(buildDoviRetention(info, sel, 442.8, rPivot, pivotTuning),
              "tuning: pivot build succeeds");
        check(rPivot != rDefault, "tuning: the mid-contrast pivot moves the curve");

        DoviCm4TrimTuning clipTuning;
        clipTuning.clipKneeScale = 0.0;   // knee pinned at 1.0 = no clipping
        check(buildDoviRetention(info, sel, 442.8, rClip, clipTuning),
              "tuning: clip build succeeds");
        check(rClip != rDefault, "tuning: the clip knee scale moves the curve");

        DoviCm4TrimTuning absolutePivot;
        absolutePivot.midContrastPivotPq = 0.5;   // the contract's other candidate
        std::array<float, 257> rAbsolute{};
        check(buildDoviRetention(info, sel, 442.8, rAbsolute, absolutePivot),
              "tuning: absolute pivot build succeeds");
        check(rAbsolute != rDefault, "tuning: an absolute PQ pivot changes the curve");
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
