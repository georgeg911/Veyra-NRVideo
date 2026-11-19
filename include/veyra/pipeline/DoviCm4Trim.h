#pragma once
// CM v2.9 / v4.0 创作 trim 算子，以及 CB1 Adaptive HDR Mapping 用到的 retention 表 r(u)。
//
// 契约（D:\DSH\.dsh-tmp\cb1-algorithms.md 第 190-285 行）给全了 r(u) 的公式：
//
//   x[k] = (10 + (M-10)*u[k]) / M                    u[k] = k/256
//   v[a,k] = trim_at_anchor((x[k]*Ta, x[k]*Ta, x[k]*Ta)).R
//   r[k] = sum_a(wa * clamp(v[a,k] / (Ta*max(x[k],1e-6)), 0.5, 1.15))
//
// 但契约明确只写了 "a neutral-axis trim operator with colour/detail controls
// disabled"，没有给出 Dolby 内部 trim_at_anchor() 的精确实现（CB1 的这部分是
// 私有 libdvbridge 代码，公开的 cb1-dve.c 只调用 dvbridge_creative_trim_rgb /
// dvbridge_creative_cm4_apply）。因此下面的算子按 Dolby CM 公开的
// slope/offset/power（lift/gamma/gain）语义实现，所有补充假设都标了 "假设"，
// 便于复核：
//
//   假设 A1  12-bit 字按 word/2048 解码（2048 = 1.0，与调用约定一致）。
//             offset / target_mid_contrast / clip_trim 的 1.0 是中性点，使用前减 1。
//   假设 A2  字值 0 视为"字段不存在"（libdovi 的缺省编码）。Dolby 规范规定缺失
//             字段推断为 2048，即中性；直接把它当字面 0 会把 slope 解成 0、整帧
//             压黑，所以在媒体路径上必须按"不存在"处理。
//   假设 A3  trim 在"以该 anchor 的目标峰值为 1.0 的归一化 PQ 码值域"上作用：
//             q  = PQ(nits) / PQ(Ta)
//             q' = pow(max(q*slope + offset, 0), power)
//             out = PQ^-1(q' * PQ(Ta))
//             该形式取自公开的 Dolby Vision 实现（MPC Video Renderer 的
//             DolbyVisionTrims()/ApplyL2Trim()：PQ 码值域上先 slope/offset 再
//             power），也是 Dolby "Lift / Gamma / Gain" 的通常读法。Dolby 官方
//             没有公开精确公式，这里不声称逐位一致。
//   假设 A4  target_mid_contrast（CM4/L8）在归一化 PQ 域里做分段幂 S 曲线，
//             枢轴取"锚点峰值的 18%"（经典中灰）经 PQ 映射后的码值：
//             pivot = PQ(0.18*Ta)/PQ(Ta)。18% 中灰以上的亮部被拉开、以下压暗；
//             枢轴点 Dolby 未公开，这是本文最需要复核的选择（另一候选是绝对
//             PQ 码值 0.5，即约 93 nits）。
//   假设 A5  clip_trim 只把正量解释为"压缩高光细节"：在 1 - 0.5*clip 以上做
//             单调软膝，越靠近 1 越平，白点略降但不越过 1。
//   只处理中性轴（三通道同值）；chroma_weight / saturation_gain 不进入本算子，
//   它们由 DoviCm4.h 的色度门控处理（契约第 209-210、252-269 行）。
//
// 本文件只依赖 DoviCm4.h 的 PQ 辅助函数和锚点结构，可纯头文件单测。

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "veyra/pipeline/DoviCm4.h"

namespace veyra::pipeline {

// retention 表的点数（契约的 257 个 knot，最后一个下标 = 256）。
inline constexpr int kDoviRetentionEntries = 257;

namespace dovi_cm4_trim_detail {

// 12-bit 字 -> 控制量；2048 = 1.0（契约给定）。
inline double word(uint16_t w) { return double(w) / 2048.0; }

// 0 是 libdovi 的"字段不存在"编码；规范规定缺失字段按 2048（中性）推断。
inline bool present(uint16_t w) { return w != 0; }

inline double slopeOf(const DoviCm4Anchor& a) {
    return present(a.trimSlope) ? word(a.trimSlope) : 1.0;
}

inline double offsetOf(const DoviCm4Anchor& a) {
    return present(a.trimOffset) ? word(a.trimOffset) - 1.0 : 0.0;
}

inline double powerOf(const DoviCm4Anchor& a) {
    const double p = present(a.trimPower) ? word(a.trimPower) : 1.0;
    // 防御非物理字值：极大/极小的幂在浮点上没有意义，也会让 r(u) 抖动。
    return std::clamp(p, 0.1, 4.0);
}

// CM4 的 midtone/clipping 控制只在 L8 锚点上存在；L8 block 长度不足时
// libdovi 把字段留成 0，所以仍然用 present() 判断。
inline double midContrastOf(const DoviCm4Anchor& a) {
    if (!a.fromLevel8 || !present(a.targetMidContrast)) return 0.0;
    return std::clamp(word(a.targetMidContrast) - 1.0, -0.9, 1.0);
}

inline double clipTrimOf(const DoviCm4Anchor& a) {
    if (!a.fromLevel8 || !present(a.clipTrim)) return 0.0;
    return std::clamp(word(a.clipTrim) - 1.0, -1.0, 1.0);
}

// 分段幂 S 曲线：amount > 0 提高中间调对比，< 0 降低；amount == 0 是精确恒等。
// 0、pivot、1 三个点固定；1 以上按同一斜率延拓（retention 采样很少用到）。
inline double midContrastCurve(double v, double amount, double pivot) {
    if (!(amount != 0.0) || !std::isfinite(v)) return v;
    const double exponent = 1.0 + amount;
    if (!(v > 0.0)) return 0.0;
    pivot = std::clamp(pivot, 0.05, 0.95);
    const double upper = 1.0 - pivot;
    if (v < pivot) return pivot * std::pow(v / pivot, exponent);
    if (v <= 1.0) return 1.0 - upper * std::pow((1.0 - v) / upper, exponent);
    return 1.0 + 0.5 * std::pow((v - 1.0) / 0.5, exponent);
}

// 高光软膝：只压缩 knee 以上，单调、不越过 1；amount <= 0 是恒等。
inline double clipTrimCurve(double v, double amount) {
    if (!(amount > 0.0) || !std::isfinite(v) || !(v > 0.0)) return v;
    const double knee = std::clamp(1.0 - 0.5 * amount, 0.0, 1.0);
    const double room = 1.0 - knee;
    if (!(room > 1e-9)) return 1.0;
    if (v <= knee) return v;
    const double over = v - knee;
    return knee + room * over / (over + room);
}

}  // namespace dovi_cm4_trim_detail

// Dolby CM 的中性轴 trim 算子：输入/输出都是绝对 nits（0..10000）。
//
// 目标峰值取自 anchor.targetMaxPq（锚点自己的目标显示域）；没有可用锚点时是恒等，
// 调用方可以放心地把未解码的 DoviCm4Anchor 传进来。非有限输入按 0 处理，
// 输出始终 clamp 在 [0,10000]，不会给调用方 NaN/Inf。
inline void doviTrimApply(const DoviCm4Anchor& anchor, const double rgbNits[3], double out[3]) {
    if (!rgbNits || !out) return;
    out[0] = rgbNits[0];
    out[1] = rgbNits[1];
    out[2] = rgbNits[2];

    const double targetPq = double(anchor.targetMaxPq) / 4095.0;
    if (!(targetPq > 0.0) || targetPq > 1.0) return;
    const double peakNits = cm4PqToNits(targetPq);
    if (!std::isfinite(peakNits) || !(peakNits > 0.0)) return;

    using namespace dovi_cm4_trim_detail;
    const double slope = slopeOf(anchor);
    const double offset = offsetOf(anchor);
    const double power = powerOf(anchor);
    const double mid = midContrastOf(anchor);
    const double clip = clipTrimOf(anchor);
    // 假设 A4：中调枢轴 = 锚点峰值 18% 的中灰经 PQ 映射后的归一化码值。
    const double midPivot = cm4NitsToPq(0.18 * peakNits) / targetPq;

    for (int j = 0; j < 3; ++j) {
        double nits = rgbNits[j];
        if (!std::isfinite(nits)) nits = 0.0;
        nits = std::clamp(nits, 0.0, 10000.0);
        // 归一化到锚点峰值的 PQ 码值域：锚点自己的白点 = 1.0。
        double q = targetPq > 0.0 ? cm4NitsToPq(nits) / targetPq : 0.0;
        if (!std::isfinite(q) || !(q > 0.0)) q = 0.0;
        // slope / offset / power（假设 A3）：先线性再幂，base 为负时按 0 处理。
        double signal = slope * q + offset;
        if (!(signal > 0.0)) signal = 0.0;
        double v = std::pow(signal, power);
        v = midContrastCurve(v, mid, midPivot);
        v = clipTrimCurve(v, clip);
        const double outPq = std::clamp(v * targetPq, 0.0, 1.0);
        double value = cm4PqToNits(outPq);
        if (!std::isfinite(value)) value = 0.0;
        out[j] = std::clamp(value, 0.0, 10000.0);
    }
}

// 契约的 retention 表。
//
// M（masterNits）不包含在 DoviCm4Info / DoviCm4Anchor 里，必须由调用方传入
// （CB1 的 source->master，取自 L1 的 PQ 最大值的 nits 值）。M 无效、锚点目标
// 不可用、权重不是和为 1 的有限非负数时返回 false，并把 r 填成中性 1.0，
// 调用方可以安全地退回"无创作 trim"路径。
inline bool buildDoviRetention(const DoviCm4Info& info,
                               const DoviCm4Selection& selection,
                               double masterNits,
                               std::array<float, 257>& r) {
    r.fill(1.0f);
    if (!info.valid() || !selection.valid()) return false;
    // CB1: master <= 10 或 > 10000 视为无效（cb1-dve.c 的 build()）。
    if (!std::isfinite(masterNits) || !(masterNits > 10.0) || masterNits > 10000.0) return false;

    struct Term {
        const DoviCm4Anchor* anchor = nullptr;
        double weight = 0.0;
        double targetPq = 0.0;
        double peakNits = 0.0;
    };
    Term terms[2];
    int count = 0;
    // 每个参与混合的锚点都必须在自己的目标峰值域里可用；CB1 用 peak>100 判废，
    // 这里只要求 peak>10，因为 100-nit 的 SDR 锚点本身是合法目标。
    const auto addTerm = [&](const DoviCm4Anchor* anchor, double weight) -> bool {
        if (!anchor) return true;
        if (!std::isfinite(weight) || weight < 0.0) return false;
        const double targetPq = double(anchor->targetMaxPq) / 4095.0;
        if (!(targetPq > 0.0) || targetPq > 1.0) return false;
        const double peak = cm4PqToNits(targetPq);
        if (!std::isfinite(peak) || !(peak > 10.0) || peak > 10000.0) return false;
        if (count >= 2) return false;
        terms[count++] = Term{anchor, weight, targetPq, peak};
        return true;
    };
    if (!addTerm(selection.nearAnchor, selection.nearWeight)) return false;
    if (!addTerm(selection.farAnchor, selection.farWeight)) return false;
    if (count == 0) return false;

    double totalWeight = 0.0;
    for (int i = 0; i < count; ++i) totalWeight += terms[i].weight;
    if (!std::isfinite(totalWeight) || std::fabs(totalWeight - 1.0) > 1e-6) return false;

    for (int k = 0; k < kDoviRetentionEntries; ++k) {
        const double u = double(k) / double(kDoviRetentionEntries - 1);
        const double x = (10.0 + (masterNits - 10.0) * u) / masterNits;
        double sum = 0.0;
        for (int i = 0; i < count; ++i) {
            const double sample = x * terms[i].peakNits;
            const double rgb[3] = {sample, sample, sample};
            double trimmed[3];
            doviTrimApply(*terms[i].anchor, rgb, trimmed);
            const double denom = terms[i].peakNits * std::max(x, 1e-6);
            const double ratio = denom > 0.0 ? trimmed[0] / denom : 1.0;
            if (!std::isfinite(ratio)) return false;
            // 契约：比例先按锚点夹到 [0.5,1.15]，再按权重求和（响应混合，
            // 不是先混合响应再夹）。
            sum += terms[i].weight * std::clamp(ratio, 0.5, 1.15);
        }
        if (!std::isfinite(sum)) return false;
        r[std::size_t(k)] = float(sum);
    }
    return true;
}

}  // namespace veyra::pipeline
