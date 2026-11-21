# Dolby Vision 支持 · 完整技术档案

> 这份文档记录杜比视界（及 HDR10+ / HDR Vivid）的**技术实现与验证结论**。
> 一次性验证数据已移入文末「已归档的验证记录」，正文只保留可复用的技术事实。
>
> **整理**：2026-11-21（精简版）｜原作者的三份旧文档已合并进本文件

---

> **文档地图**（我自己的文档，作者原版 600+ 份在 `docs/` 不动）
>
> | 文件 | 一句话定位 | 什么时候看 |
> |---|---|---|
> | **MY_WORK.md** | 项目现状、定制功能、行动规则 | 新会话第一步 |
> | **HANDOFF.md** | 构建、部署、验证手册 | 要构建或部署时 |
> | **docs/DOLBY_VISION.md** | 杜比视界 P5/P7/L1 技术档案 | 改杜比视界相关代码 |
> | **docs/HISTORY.md** | 技术排查链（按日期） | 遇到具体技术障碍 |

## 目录

| 想查什么 | 看哪节 |
|---|---|
| 技术事实、P5/P7/P8/P10、L1 跟随、踩过的坑 | 一、已实测的关键事实与陷阱 |
| P7 增强层的数学算法（ETSI 规范） | 二、P7 双层合成算法 |
| 相关源码在哪 | 三、代码位置索引 |
| HDR10+ 元数据怎么读、三个 bug | 四、HDR10+ 动态元数据 |
| HDR Vivid 识别与 P8.4 路由 | 五、HDR Vivid 与 Profile 8.4 |
| 旧的验证数据在哪 | 附、已归档的验证记录 |

---


## 一、已实测的关键事实与陷阱

> ⚠️ **本节写于 2026-10-17，其中的「当前状态」已经过期。**
>
> - 那时写「全部只在本地 custom 分支，未推送」——**现在已提为 PR #13 / #56**
> - 那时列出的提交哈希（`e1d39c93` 等）是历史节点，最新状态看 `git log`
>
> **最新的 PR 与合并状态**记录在本地工作笔记里，不在本仓库。
> 本节保留的是**为什么这么做、技术上怎么验证的**，这部分不会过期。

新会话从这里开始读，不要从零摸索。所有结论都经过实测或编译验证。

### 2. 已实测的关键事实（含本轮新增）

- FFmpeg 的 AV_FRAME_DATA_DOVI_METADATA 确实到达帧；`VEYRA_DOVI_TRACE` 下日志 dovi-probe present=1
- L1 提取正确：p8_30s.mkv → maxPq=0.6339（dovi_tool 金标准 2596/4095）
- PQ→nit：0.6339 PQ = 336 nit；0.85×336 = 286，实测 highlightStartNits=286
- **曲线必须打开**才有可见效果：`hdrCurve.enabled=0` 时 blit 的 curveFlag=0，dimensions 不参与
- **`describeStages()` 是唯一权威**：`EnhanceGraph::initialize()` 里 `desc_=desc` 会整体覆盖描述，
  所以任何"新加的 live 字段"必须写进 `include/veyra/engine/GraphDescription.h` 的 `describeStages`，
  否则只在 applySettings 时活一下，图形一重建（开片、后端恢复）就丢。曲线的 hdrCurve 就在这里。
- 会话/预设库的 tuning 块**不要**用来持久化这类开关：bridge 的 `revalidate()` 会用 runtime configuration
  重写 pending settings，块里的值会被打回默认（hdrCurve 当年就是靠 `commitHdrTuning()` 事后重写才活着）。
  本轮试过把 doviFollow 放进 ChainGlobalSettings 并把 library→v14 / session→v9，实测会话被拒
  （`[ERROR] [ui-session] 模式会话损坏、无法读取或版本不支持`），已全部回退，改用偏好持久化。
- 可用的持久化通道（实测有效）：`qml-preferences.v1.json` 的 `doviFollow`（全局兜底）
  + `hdrTitles[].doviFollow` / `hdrProfiles[].doviFollow`（每片/每显示，和曲线同一份记忆）
- 手写 chain-session.v1 当夹具不可靠：v1 会话只有 9 个字段/配置，加字段必须整份改成新版本，
  写错就被拒且**保留原文件不覆盖**，现象是"设置没生效但文件没变"
- **`peakCapNits` 同时是中灰 gamma 的锚点**（`hdrCurveLuminance`：g=log(mid/peak)/log(203/peak)），
  所以跟随**绝对不能**改它。用真实档位（mid 175 / start 450 / cap 1200）实测：把 cap 跟着
  帧峰值 304 改，1 nit 掉 78%、100 nit 掉 18%、1000 nit 被硬夹掉 61% —— 整张画面变暗，
  而且逐帧 cap 波动会让画面呼吸。现行实现只改 `highlightStartNits`，且**单向下调**
  （用户起点比帧峰值低时保留，不削弱用户已设的压缩）。
- 滚降区间（dims[12]）在跟随时用"内容自身峰值"收口，但 `dims[13]`（硬上限）仍是用户值：
  否则高显示峰值（例如系统报 10000 nit）会把坡拉得极缓，等于没压。
- 实测口径（本机日志）：p8_30s.mkv 内容峰值 336 nit、用户档位经显示峰值缩放后
  start=2000 / cap=4000；跟随生效后 start=286（n=0）、273（n=200, 帧峰值 321），cap 恒为 4000

### 2.5 P5（P7 双层）已探明的实现事实（2026-10-17）

用 `D:\share\Dolby Vision\[Dolby] ... Full Enhancement Layer Test`（Dolby 官方 FEL 测试片）实测：

- 容器：`profile 7, level 6, RPU 1, EL 1, BL 1`；dovi_tool demux 出 BL 67.2 MB / EL 44.7 MB
  （EL = BL 的 66% → FEL，不是 MEL）
- **FFmpeg 完全看不到 EL**：`demuxer: opened streams=1`、packet 里 `layer=0` 的 NAL、
  `sideDataElems=0`。所以 `AV_FRAME_DATA_DOVI_METADATA` 只有 RPU，没有 EL。
- **但 EL 一直在我们拿到的 packet 里**，以 **NAL type 63**（Dolby 私有，未指定类型）与 BL 交织：
  ```
  7e 01 + 完整标准 NAL        ← 这就是包装规则
  7e01 4001… = 内层 type 32 (VPS)
  7e01 4201… = 内层 type 33 (SPS)
  7e01 4401… = 内层 type 34 (PPS)
  7e01 4e01… = 内层 type 39 (SEI)
  7e01 2601… = 内层 type 19 (IDR slice)
  7e01 0001… = 内层 type 0  (TRAIL_N)
  ```
  RPU 是 **type 62**（`7c 01` 包装，每 AU 一个，约 376 字节）。
- **EL 的 parameter sets 不在 hvcC 里**，而在 TrackEntry 的 `0x41E4 BlockAdditionMapping`：
  `0x41E7`=名称（"hvcE" / "dvcC"）、`0x41ED`=数据。hvcE 是 hvcC 格式（795 字节，含 VPS/SPS/PPS）。
- 剥掉 `7e 01` 后 EL 可直接解码：**1920x1080 yuv420p10 pq**（BL 是 3840x2160）。
- 单独解码 EL 会报参考帧警告（`Could not find ref with POC n` / `First slice in a frame missing`），
  但**能连续出帧**；用 dovi_tool 拆出的 EL.hevc 做对照，错误数**完全相同（12060）**，
  证明我们的提取与它等价，这些警告是残差层固有现象。
- 提取器原型：`D:\DSH\.dsh-tmp\dovi-follow\{make_el_test,extract_el_full,probe_el_subtype,naldump}.py`
- mpv 的旁证：它有 `Dolby Vision Profile 7 splitter` + `enhancement_pair`，但实测在同一个文件上
  配对失败（`el_newer; el head 1.752000`），全程只发 BL → **连 mpv 也没真正用上 EL**；
  libplacebo 的 dolbyvision.c 注释也明说 P7 只用 L1 元数据。

剩余工作（P5.3/P5.4）：
1. 在 MediaFileSource 里按需提取 type 63（剥 `7e01`）+ 拆出 hvcE 作为 EL 解码器 extradata，
   建第二条解码路径（1080p → 需要 2× 上采样对齐 BL）
2. GPU 合并：用 RPU 的 reshaping 参数（FFmpeg 的 `AVDOVIRpuDataHeader` 已解析：
   `coef_log2_denom` / `bl_video_full_range_flag` / `el_bit_depth` / piece-wise 曲线 / MMR）
   做 inverse reshape + EL 残差相加 → 12bit
3. 任何一步失败/性能不足就回落到现有 BL+RPU 路径，并用 FEL Test/Control 两片验证


### 2.6 P5 最终状态（2026-10-17 收尾）

P7 双层增强层**已经实现并跑通**，默认关闭，界面开关在
专业页 → 杜比视界设置 → 「合并增强层（Profile 7 双层）」。

| 子项 | 状态 | 证据 |
|---|---|---|
| P5.1 EL 在哪 / 怎么封装 | ✅ | EL 在 NAL type 63，`7E 01` + 标准 NAL；参数集在 TrackEntry 的 `hvcE`（不在 hvcC） |
| P5.2 EL 独立解码 | ✅ | 1920×1080 yuv420p10le；1200 帧 / 1238 包；0.07 ms/包；残差实测 −13…+17 |
| P5.3 GPU 合并 | ✅ | 开关开启后画面逐帧改变 0.23~0.29（整体 1.2%），关闭时**逐位相同** |
| P5.4 回落 | ✅ | Profile 8 / Control 片：`bound=0`，零 EL 活动，播放正常 |

#### 实现位置

- `src/source/MediaFileSource.cpp`：`extractEnhancementNals` + `feedEnhancementLayer`
  （第二条软解解码器，容错模式），`out.doviEl` / `out.doviP7`
- `include/veyra/pipeline/DolbyVisionP7.h` + `DoviP7Constants.h`：参数打包进
  `dv0..dv6`（`dv6.w=2` 表示重建、`=3` 表示重建并把残差画成灰度）
- `include/veyra/engine/DoviBlElCompose.h`：CPU 参考实现（7 条单测锁住）
- `shaders/YuvToLinearRgb.hlsl`：`ReconstructLumaP7`（t12 = EL luma，R16_UINT）
- `src/pipeline/EnhanceGraph.cpp`：`uploadElLuma` + 绑定 slot 12 + 参数写入

#### 三个把人骗过的坑（都会伪装成"功能没生效"）

1. **`dv6.w` 写成了 `params[24]`**（那是 dv6.x）。着色器读到的永远是 0，合成从不执行；
   而单测断言写的是同一个错槽位，所以一直是绿的。**教训：断言要对着语义写，不是对着实现。**
2. **着色器从未部署**：只复制 exe，`.dxil` 还是旧的（20596 vs 21808 字节）。
   **教训：部署清单必须包含 `shaders/*.dxil`。**
3. **播放位置记忆**：第二次运行从上次位置继续，两次画面完全不同步，对比毫无意义。
   **教训：自动化对比必须用全新 data-dir（清空 `positions`）。**


#### P5 的最终结论（2026-10-17 收尾）

配对机制**已经打通并验证**：`read()` 会压住最多 3 个基础帧，等它们各自的增强层帧按
时间戳到达后一起交付；配上就合成，等不到就退回基础层。日志证据：

```
PAIRED held pts=542 el=1
[dovi-el-gpu] bound=1 frames=3 ready=1 el=1
```

**但配对率被增强层的出帧率卡死了。** 解码器实际吐出的时间戳是跳跃的：

```
83 → 250 → 292 → 334 → 501 → 542 …      缺 125、167、209、375、417、459
```

基础层每帧都有，增强层只能解出其中一部分（实测不到 20%），而且加大缓存/等待窗口
（8→32、3→8）**完全没有提升配对率**——缺的帧就是没有。

由此可以断定：

| 方案 | 能否解决 |
|---|---|
| 放大配对窗口 | ❌ 已实测无效 |
| B′（包队列 + 预读） | ❌ 改的是喂包节奏，改不了丢帧 |
| B（独立解码线程） | ❌ 同上 |

丢帧的根因在增强层流的解码本身（参考帧链断裂，FFmpeg 跳过不完整参考结构的整帧），
不在取流架构里。**除非换解码路径，否则画质收益的上限就在这里。**

因此：功能保留（机制、代码、文档齐全），**开关默认关**。将来若 FFmpeg 的 EL 解码
改善，这套配对机制可以直接受益，不需要重做。

本轮还修掉两个把功能伪装成"不生效"的 bug，都值得记住：

1. **`doviP7Compose` 的偏好恢复被嵌在 `doviFollow` 的条件分支里**——偏好文件里没有
   `doviFollow` 时，这个开关永远读回 false。所有测试目录都因此失效。
2. **参数打包挂在"本帧有 EL"的条件上**——配对移到上层后，打包时 packet 里永远没有
   EL，`ready` 恒为 0，即使配上也不合成。


#### 2026-10-28 修复：崩溃根因与配对率（P0 + P1）

2026-10-17 的结论是"配对率被 EL 出帧率卡死"，这个判断**是错的**。真实原因有两个，
都在我们自己的代码里。

**P0：d3d11va 表面池被配对帧泄漏耗尽（崩溃，`code=-12`）**

`read()` 为等 EL 把基础帧 `av_frame_clone` 后放进 `baseWaiting_`，交付时把裸指针交给
调用方却**从没释放**。每个克隆都 ref 住一片 d3d11va surface，而该解码器的表面数组是
**固定大小**的（日志实测 `array=20`），Veyra 又从未设过 `extra_hw_frames`：

```
[media] decoder: d3d11va decoded surface 3840x2176 ... array=20 slice=19
[engine-stall] frame=12 ...
[media] decoder: send_packet failed code=-12 text=Cannot allocate memory
```

20 片里 DPB 常驻约 8 片，可周转 12 片，正好等于崩溃前的 12 帧。上游 mpv 在
`video/decode/vd_lavc.c` 里把这件事写得很直白（"every frame ... pins a surface from
the fixed pool"），并给 BL 解码器预留 6+8 片；我们没有。

修复：交给调用方的克隆在**下次 `read()` 入口**释放（调用方本来只借到下一次读，
`EngineController::retainCandidate()` 会先自己拷一份）；seek、硬件→软件回退、close
时一并清理；P7 启用时给硬解预留 10 片（实测池子从 `array=20` 变成 `array=30`）。

**P1：配对永远差一帧，而且过期判断用错了基准**

EL 解码器按设计比基础解码器**慢一帧**（送包 N 时解出的是 N−1 的帧）：

```
attempt base_pts=42  cached=[0]      ← BL 42 在等，缓存里只有 EL 0
attempt base_pts=83  cached=[42]
attempt base_pts=125 cached=[]       ← 0 和 42 都被当成"过期"删掉了
```

新增的"丢弃过期 EL"用了**当前正在探测的 BL 帧**作基准，于是把**队列里更老的 BL 帧
还需要的那一帧**删掉了，谁也配不上。改成按 `baseWaiting_.front()`（最老等待帧）判断，
并把配对容差从精确相等改成 2 tick。

**P1：改用 FFmpeg 自带的 `dovi_split` 拆流**

手写 walk 只能在"参数集恰好出现在流里"时工作，seek 之后无法恢复。FFmpeg 9.0 的
`dovi_split` BSF 做同样的事，并且额外：从容器的 `hvcE` 记录取 EL 参数集
（Matroska BlockAdditionMapping → `AV_PKT_DATA_HEVC_CONF`，libavformat 已经帮我们挂好）、
按 profile 折半算 EL 尺寸、无 EL 的 AU 返回 `EAGAIN`。容器没有 hvcE 时自动回退到手写 walk。
另外：RPU 的 `disable_residual_flag` 连续 24 帧为 1 时判定为 MEL，直接关掉 EL 解码。

**实测（4K P7 FEL 测试片，120.036 秒）**

| 项目 | 修复前 | 修复后 |
|---|---|---|
| 播放 | 12 帧后 `-12` 崩溃 | 全片播完，0 次 `-12`、0 次回退 |
| EL 拆流 | 手写 NAL walk | `dovi_split filter ready mode=el EL 1920x1080 extradata=795 bytes` |
| 配对 | 多数配不上 | `PAIRED held pts=…` 正常，`bound=1 frames=800` |
| seek | 未验证 | 8 次精确跳转，每次 2 帧内重新配上 |

仍未做：FEL 重建只有亮度通道（上游 libplacebo 是三通道 + pivot 多项式 + 12bit），
`DoviBlElCompose.h` 里的 CPU 参考实现有完整 MMR，尚未移植到 HLSL。


#### 2026-10-28 P2：三通道残差与 MMR 逆映射

P0/P1 之后能播了，但合成只加**亮度**残差，色度是原样透传。抓一帧 RPU 看就明白这不成立：

```
curve 0 (Y) : numPivots=9  mapping=poly(1)  coef=(0, 1<<23, 0)   → identity
curve 1 (Cb): numPivots=2  mapping=MMR     mmrOrder=3
curve 2 (Cr): numPivots=2  mapping=MMR     mmrOrder=3
nlq: offset=512 slope=2048 threshold=0 inMax=1048576 （三个分量相同）
```

两个事实：
1. **三个分量各有自己的 LINEAR_DZ 量化器**，残差必须按分量分别反量化；
2. **色度的逆映射是 MMR 多项式**，不是恒等。以 Cb 为例，权重是
   `0.762·c + 0.381·c² + 0.1905·c³`（`coef_log2_denom = 23` 归一化后），
   在 0.5 附近几乎不变、离中心越远越偏离恒等。

实现：
- 参数布局改成每分量一组 NLQ（`dv0..dv2`），`dv3` 放位深/分母，`dv4/dv5/dv6` 放 MMR 常数、
  模式与阶数；
- MMR 权重放不下常量池（60 个已经用完），改为 **18×1 的 RGBA32F 系数纹理（t14）**，
  每帧上传 288 字节；
- 色度平面上传到 **t13（R16G16，960×540）**，与 luma 采样一致地做双线性上采样后取整；
- pass 的额外 SRV 范围从 `t8..t12` 扩到 **`t8..t14`**（这条没改会导致
  `CreateComputePipelineState failed constants=60 extra=5`，shader 里引用了范围外的寄存器）。

`ReconstructP7` 现在是：分量级 NLQ 反量化 → 分量级逆映射（identity 或 MMR）→
三者相加 → 12bit 量化。与 libplacebo 的 `sh_dovi_compose_nlq` + `sh_dovi_reshape`
同构，差别只在我们要求「单段 MMR」或「恒等」，其它曲线仍回退 BL。

**实测**：`reconstruction parameters packed nlqOffset=512 slope=2048 coefDenom=23 modes=0/1/1 mmrOrders=0/3/3`
与 RPU 完全对应；残差图（`VEYRA_DOVI_P7_DUMP=1`）能看到城市夜景的高频细节，
不是全屏过曝，说明 NLQ 的量级正确。

仍未做：多段曲线（每个分量 >2 个 pivot 且分段不同）与 `ext_mapping_idc != 0` 的片源仍回退 BL。

#### 2026-10-28 复检：打包对了，读错了

P2 的「实测通过」被自己推翻了一半。复检 1013 行改动时发现两个缺陷。

**缺陷一：Cb 的 MMR 逆映射从未执行**

着色器拿 **luma 的 mode 标志**去判断要不要处理 Cb，又把结果写进 Cb 的槽：

```hlsl
// 错的
if (dv5.y > 0.5) base.y = MmrEvaluate(1, sig, dv4.z, int(dv6.y + 0.5));
if (dv5.z > 0.5) base.z = MmrEvaluate(2, sig, dv4.w, int(dv6.z + 0.5));
```

`dv5.y` 是 mode[0]（Y 的）。本片源 Y 是恒等映射 → `dv5.y == 0` → **整个分支不进**，
Cb 一直按恒等处理；Cr 进了，但 constant 取的是 Cb 的。正确写法是每个分量读自己的三元组：

| 分量 | mode | constant | order |
|---|---|---|---|
| Y | `dv5.y` | `dv4.z` | `dv6.x` |
| Cb | `dv5.z` | `dv4.w` | `dv6.y` |
| Cr | `dv5.w` | `dv5.x` | `dv6.z` |

**为什么当时没发现**：日志打的是 `modes=0/1/1 mmrOrders=0/3/3`——**打包侧完全正确**；
而消费侧错位后，画面依然正常，因为
(a) 本片源三个分量的 `mmr_constant` 都是 0，
(b) Cb 的映射 `0.762c + 0.381c² + 0.1905c³` 在 0.5 附近近似恒等，只在两端才偏。

**教训：参数日志只能证明"打包对"，不能证明"消费对"。** 要证明后者，得让参数处在
会让错误显形的位置——比如拿一张 mid-grey 之外、三通道 constant 非零的帧去比。

**缺陷二：引擎缓存了借用的 EL 帧**

`out.doviEl = elFrame_` 是借出指针，而 `elFrame_` 会在下次配对、MEL 关闭、解码失败时
被 `av_frame_free`。引擎的 `retainCandidate()` 却把整个 `pkt`（含该指针）存进
`cachedPacket`，稍后重放时再传给 `graph->process(...)` → 可能读已释放内存。

触发窗口很窄（要 `initialFileFramePending`，而 P7 内容通常是 HDR，走不到
`pausedEligible` 那条重放路径），但确实存在。修法：缓存时把 `doviEl` 置空，重放那一帧
退回基础层，视觉上是一帧的事。

**教训：借用指针一旦被上层缓存，生命周期就不再由出借方控制。** 要么在交接处收窄
（这里是置空），要么把所有权真正转移出去。

#### 多段曲线：量完之后决定不做（2026-10-28）

P2 只支持「恒等」和「单段 MMR」的逆映射，多段曲线一律回退基础层。当时判断是
"手头没有多段片源，做了也无法验证"。后来加了按帧统计（`P7 mapping frames`），
三个片源的结果：

| 片源 | 类型 | 被拒帧 | 比例 |
|---|---|---|---|
| `FEL TEST ST DL P7 CMV4.0 4000nits V4` | 测试片 | 0 / 600 | **0.0%** |
| `FEL RPU_EL测试` | 测试片 | 4 / 300 | **1.3%** |
| `Despicable Me 2010` UHD remux | **真实电影** | 9 / 600 | **1.5%** |

**测试片和真实盘的结论一致：多段帧约 1.5%，即每 60–70 帧一帧、大约每 3 秒一次。**
1.5% 的帧不合成，视觉上是一次极轻微的跳变，不构成"画面不一致"。

所以：**不做多段支持**（成本 300–400 行，收益 1.5% 的帧）。这不是偷懒，是量过之后
的取舍。如果以后遇到整片使用多段曲线的片源（`refused` 比例很高），计数器会立刻暴露，
那时候再做，而且有现成的验证素材。

顺带纠正一个误判：日志里 `identity ↔ multi-piece` 来回刷屏，看起来像"整段交替"，
那是因为**只在状态变化时打印**。实际计数只有 1–2%，"看起来一半"是日志形态造成的错觉。

#### L5 有效区域：查到了，但不动（2026-10-28）

DV 的 L5 扩展块描述"有效画面矩形"，mpv 用 `dovi-level5-mode` 暴露它（`mask` 涂黑 /
`crop` 裁掉）。实测两个片源：

| 片源 | L5 active area | 有效画面 |
|---|---|---|
| `[Dolby] FEL Test` | top=275 bottom=276 | 3840×1609（2.39:1） |
| `Despicable Me 2010` | top=42 bottom=42 | 3840×2076（1.85:1，几乎无黑边） |

结论：
- **`mask` 没有收益**：Veyra 是 HDR/PQ 输出，黑边在 PQ 域是 0，本来显示就是纯黑，
  不像 SDR 映射那样会被抬成灰。
- **`crop` 只是显示偏好**：把 2.39:1 的画面裁边填满窗口会改变构图，多数播放器默认不裁。
  真要做是 ~50 行（把 L5 传给 presenter，`videoRect` 按有效区比例算），默认关闭。

所以现在只记录、不应用。数据留在日志里，需要时再说。

#### Profile 5 实测（2026-10-29）

P5 一直是本项目里唯一只有代码审查、没有真实数据的主路径。找到片源后补上了：

片源 `Wednesday S02E01 2022 2160p NF WEB-DL DDP5.1 Atmos DV H.265`（Netflix，8.2 GB）。

```
[source-dovi] profile=5 level=6 compatibility=0 bl=true el=false rpuDeclared=true route=2
[source-dovi] base layer pinned to PQ for profile 5
[source-dovi] profile 5 base layer converted as IPT-PQ-C2: RPU reshaping (per-frame,
              projected on {1,sqrt(x),x}) -> RPU ycc_to_rgb -> PQ EOTF
              -> RPU rgb_to_lms x fixed HPE LMS->BT.2020 -> PQ OETF
[source-dovi] profile 5 constants ycc_to_rgb=[1.000, 0.0975, 0.2052 | ...]
              lms_to_rgb=[3.2388, -2.3256, 0.0867 | ...]
```

画面色彩正常（若 IPT-PQ-C2 映射出错会呈现明显的绿屏或紫屏），日志 0 错误。

顺带修掉两处日志缺陷：

| 问题 | 后果 |
|---|---|
| 钉基底时无条件打印 "8.1 is PQ" | profile 5 的日志在说 8.1 的事 |
| profile 5 也打印 P7 的曲线诊断 "supported" | 打包器被短路，report 是默认构造值，相当于对一条没有增强层的流断言"曲线可用" |

构造伪 P5 的办法（改容器 DoVi 配置记录，用于验证 P5 的守门逻辑，不验证转换）：
把 MKV 里 `dvcC` 之后 7 字节起的 `DOVIDecoderConfigurationRecord` 改成
`profile=5, el=0, bl=1, compatibility=0`。这样 Veyra 会进入 P5 路径，并因为
RPU 仍是 P7 的、没有颜色块而正确拒绝——这正好验证了"不硬转"的防御逻辑。

#### Profile 8 的基底类型由 compatibility ID 决定，不是 profile 号（2026-10-29）

常见的混淆是把 8.2 和 8.4 对调。按 Dolby 的 bitstream profiles 文档 Table 2：

| CCID | 标签 | 基底 |
|---|---|---|
| 0 | Dolby Vision proprietary | P5 |
| 1 | HDR10 | P8.1 |
| 2 | **SDR**（BT.1886 / BT.709） | **P8.2** |
| 4 | **HLG** | **P8.4** |
| 6 | Blu-ray | P7 |

所以：

- **iPhone 录的 Dolby Vision 是 HLG 基底，即 Profile 8.4**
- **SDR 兼容基底是 Profile 8.2**

代码按 CCID 判断而不是 profile 号（`source/DolbyVision.h` 的 `route()`），并
据此钉住色彩标签：`matches()` 要求 HLG 是 `transfer==HLG && BT2020`、SDR 是
`!isHdrPath() && BT709`。

实测印证：B 站那部 Galaxy 是 `profile=8 compatibility=4`，日志为
`base layer pinned to HLG`。

一个细节：官方 Table 1 里 Profile 8 的 CCID 只列 "1 or 4"，CCID 2 在官方对应
Profile 9（AVC 8-bit）。但实践中确实存在 `profile=8 compatibility=2` 的文件
（Jellyfin 也接受 1/2/4），所以「P8.2」是实践叫法。按 CCID 判断两种情况都能
正确处理。

**验证路由的伪造办法**（手头没有 P8.2 片源时）：把任意 DV 文件的 `dvcC` 里
`DOVIDecoderConfigurationRecord` 改成 `profile=8, el=0, bl=1, compatibility=2`，
Veyra 就会进 SDR 分支。判定只看两行日志：

```
[source-dovi] profile=8 ... compatibility=2 ... route=3     ← 3 = DolbyBaseLayer::Sdr
（并且没有 "base layer pinned to" 那一行：SDR 基底不钉 HDR 标签）
```

画面会错，因为伪造文件的基底其实是 PQ，被按 BT.709 解释。所以这只能验证路由与
色彩钉选，**不能**验证 P8.2 真正的价值——它的 RPU 会把 SDR 基底**提升**成 HDR。

**这部分没有实现。** P8.2 目前的定位是"兼容播放"：按非 DV 设备的方式显示 SDR
基底，不应用 RPU。和 P8.1 是同一类取舍，只是损失更大（P8.1 的基底本身接近 DV
效果，P8.2 的基底只有 SDR）。要做提升需要一套 SDR→HDR 的 reshaping，规模和 P7
的三通道重建相当，且同样没有片源可验。

#### Profile 10（AV1 里的杜比视界）：能播，但 FFmpeg 还差两处（2026-10-29）

P10 是 AV1 封装的杜比视界（`dav1.10.xx`），元数据在 **AV1 的 T.35/EMDF OBU** 里，
不是 HEVC NAL。MP4 里的 sample entry 是 `dav1`，内部结构是 `av1C` + `dvvC`。

**测试素材**：Dolby 官方在线交付套件的 P10 测试流（AV1 10-bit，47 秒，25fps，
480p/720p/1080p/4K 四档，国内可直连、无 DRM）：

```
https://ott.dolby.com/OnDelKits/Dolby_Vision_Online_Delivery_Kit/v1/test_signals/clear/P10_25/master.m3u8
```

### 修掉的 bug：CCID 0 被当成「不支持」

这个测试流的 `dv_bl_signal_compatibility_id = 0`（Dolby 定义为 "Dolby Vision
proprietary"），而路由当时只映射 1/2/4，落进 default 分支直接拒绝播放。

但 Dolby 的 Table 2 给出 Profile 10 在 CCID 0 时的 VUI 是 `1, 9, 16, 15, 1` ——
**16 = PQ、9 = BT.2020**，也就是 PQ 基底、只是不声明兼容性。现在路由到 HDR10
并钉 PQ。

### 两个 FFmpeg 上游缺口

| 缺口 | 表现 | 现状 |
|---|---|---|
| `dav1` sample entry 没被 `isom_tags` 映射 | demuxer 报 `codecId=0`，完全无法解码 | **重封装绕过**：把 init 里的 `dav1` 改成 `av01`，立即能播（payload 本就是 av01 + dvvC） |

**只有这一个缺口。** `av01` 封装的 P10 是完全可用的：`dvvC` 配置读得到，
**AV1 OBU 里的 RPU 也读得到**（实测 `extension blocks=5 levels=[6,1,2,4,5]`，
即 L1/L2/L4/L5/L6 全在），L1 跟随也能跑（`source=DV10`）。

> 更正：早先从一个 480p、fourcc 被改写过的文件只看到配置、没看到 RPU，据此写过
> 「FFmpeg 不解析 AV1 的 DV RPU」，那是错的。用 1080p 原始 `av01` 文件复测，
> RPU 完整可读。

### 2.7 HDR 曲线被假峰值稀释（2026-10-17 修复）

**症状**：调 HDR 曲线（高光保护、整体观感）感觉没有变化。

**根因**：`DisplayPeakInfo::plausible` 是 `valid` 的副本——注释里描述的"10000 nit 配
0.000 黑位是 EDID 上限而不是测量值"这条检查**从来没写**。于是：

```
DXGI 报 10000 nit  ->  plausible 仍为 true  ->  presenter 注入 10000
                   ->  resolveHdrTuning 按 10000/1000 缩放曲线
                   ->  用户设的 450/1200 实际变成 2000/4000
                   ->  内容峰值只有几百 nit，曲线永远不触发
```

**修复**：
- `PresentSink`：`plausible = valid && !(peakNits >= 4000 && minNits < 0.01)`
- `VideoPresenter`：只有 `valid && plausible` 才注入 `displayPeakNits`
- 判为不可信时不缩放（`displayPeakNits` 保持 0），曲线按用户原值生效
- 界面明确标注"这个数值是显示器的上限声明，不是实测，因此曲线未按它缩放"

**本机实测**：`dxgi output: peak=10000 black=0.0000 valid=1 plausible=0`，
`curveDisplayPeakNits=0`（此前是 10000）。

#### 顺带做的界面改进

- 杜比视界卡片 summary：`Profile 7 · RPU 有` → `Profile 7（双层）· 最亮 1000 nit`
- 卡片内新增「当前片源的推荐」一行（复用曲线卡片的 `applyRecommendedHdrTuning`），
  并加门控：**非杜比视界片源时不可点**（HDR10 同样带亮度元数据，一开始漏了这个判断）
- 用户档案的 5 条影片记忆统一为一套通用基准：参考白 203（BT.2408）、高光起点 450、
  上限与显示峰值 800、滚降 110、强度 100；原文件已备份

#### 一项评估后决定不做

「跟随效果反馈」（在界面上显示当前帧跟随压到了多少 nit）。它是可观测性而非功能，
需要跨 presenter / snapshot / bridge 四层，且每帧变化的值不能直接推给 QML。
对普通使用没有价值，开关对比更直观。


#### 增强层覆盖率上限：实测验证（2026-10-17）

之前记录的"约 3%"是按基础层 2877 帧当分母算的，**分母用错了**。对 EL 流本身做
NAL 统计（`ffmpeg -f hevc` 提裸流 + 解析 Annex B 的 nal_unit_type）后：

| 指标 | 数值 |
|---|---|
| EL 流访问单元（帧）总数 | **1472** |
| IRAP（可独立解码的 I 帧） | **85**（IDR 30 + CRA 55） |
| 实际解出 | **87** |
| 每帧 slice 数 | 8 |

- **每帧 8 个 slice**。统计 NAL 时会得到 240 个 "IDR"、440 个 "CRA"，那是 **slice 数**
  除以 8 之后的 30 帧和 55 帧——早期把 slice 当帧，才误以为"理论上限 240 帧"。
- 按 EL 自己的 1472 帧算，I 帧占 **5.8%**；实际解出 87 帧 ≈ 85 个 I 帧（多出 2 帧是
  CRA 偶尔成功），即 **100% 的 I 帧都解出来了**。
- 剩下的 1387 帧是 P/B，需要基础层的参考帧，单流解码器物理上无法满足。

**结论：5.9% 就是上限，不是实现缺陷。** 这也可以直接答复"为什么只多出一点点画面"
的疑问。

#### 一次失败的性能优化（不要重做）

动机：94% 的 EL 帧注定解不出来，却仍被送进解码器，浪费 CPU。

做法：在 `extractEnhancementNals` 里只保留 IRAP slice（16–21）和参数集（32–35）、
SEI（39/40），丢弃 P/B（0–9）。

结果：**解码器 `decoded=0`，一帧都出不来**，配对数从 8 掉到 1。推测是参数集顺序
或 slice 间上下文被打乱（PPS 每帧一个，共 1556 个）。回退后立即恢复正常。

**以后不要再试这个方向**：收益是"省 CPU"而不是"多出画面"，而完整验证需要解析
slice header，成本远超收益。

#### 仍未定论的两条（不要当成已完成）

- **EL 覆盖率矛盾**：离线统计 mkv block 得 51%（`111000` 节奏），运行时
  `decoded/packets` ≈ 97%。大概率是当初的 block 统计口径漏了 BlockGroup，
  但没有重新统计确认。当前实现按"这一包是否解出 EL 帧"判断。
- **画面方向**：CPU 原型预测 +6% 变亮，实测某段 −1.2% 偏暗。残差围绕零点对称
  （−13…+17），不同片段方向不同，原理上说得通，但**没有第二份独立实现交叉验证**。

#### 诊断开关（默认全关）

```
VEYRA_DOVI_P7_DUMP=1   着色器把 EL 残差画成灰度（黑 = 该帧无残差）
VEYRA_DOVI_TRACE=1     逐帧 RPU + 合成绑定计数
界面开关                专业页 → 杜比视界设置 → 合并增强层
```

### 4. 绝对不要再踩的坑（每一条都真踩过）

1. 不要用「找右花括号当块尾」删代码 —— 曾因此误删 23 行 DV 解析代码。删改一律完整多行精确匹配，删完 git diff 核对。
2. PowerShell 的 Out-File -Encoding utf8 会写 BOM —— BOM 插到文件中段会变非法 token，整个 QML 组件加载失败。写文件用 Python / WriteAllText(UTF8Encoding(false))。
3. qmllint 0 错误不等于能加载 —— 唯一可靠关卡是启动退出码 0，且日志无 Unexpected token / no root object / Type unavailable。
4. hdr_tuning 与 preset_library 测试有状态残留：%TEMP%\pr19-hdr-persistence 与 %TEMP%\veyra-preset-library-tests
   里的旁文件不会被清，第二次运行就会假失败。跑之前把这两个目录**移走**（不要直接删，按约定进回收站/另放）：
   移走后 100% 通过，本轮实测 hdr_tuning / preset_library 均如此。
5. MediaFileSource.cpp 里 `auto& dv=info_.dolbyVision;` 出现两次 —— 定位必须用多行唯一锚点。
6. **`out.dovi = doviFrameInfo;` 曾经写在提取之前**，导致每帧带的是上一帧的 L1（曲线慢一帧，首帧没有值）。
   本轮已改到提取之后；改这类顺序时注意 P5 分支也会重算同一个成员（值相同，无副作用）。
7. 验收前必须把 exe（exe + .p5）与 qml/qml.p5 一起部署。
8. 绝不自作主张 push。
9. **跟随绝对不能改 `peakCapNits`**（它就是中灰 gamma 的锚点）。第一版改了，用户一眼看出
   "整张画面都在变"；用他的真实档位算出来暗部掉 78%。现在有 3 条契约测试锁死：
   上限原样保留、1~203 nit 输出完全一致、不削弱用户设得更低的起点。
10. 滚降判断发生在中灰 gamma **之后**（顺序 lift → mid → rolloff → cap）。用户的 mid=175 会把
   336 nit 的内容先压到 ~297，所以"跟随"在他的档位上只能做到 -0.6% 量级 —— 这不是没生效，
   是曲线的既有形状决定的；想验证"只动高光"，看暗部/中间调是否纹丝不动，别指望高光大幅变化。
11. 本机系统把显示峰值报成 10000 nit 级，档位里的 start 450 / cap 1200 会被 `resolveHdrTuning`
   缩放到 clamp 上限（2000 / 4000），静态曲线对 336 nit 的内容本来就空转 —— 排查"跟随没反应"时先看
   日志里的 `(user=…)`，别拿 profile 里存的原值当运行时值。

已确认与本工作无关、但会出现在日志里的噪音：
- `[ERROR] [qml] DialogHost.qml:767 / 1219 Unable to assign [undefined] to bool` —— 改动前就有（04:59 的日志里已出现）
- `veyra_settings_transaction_tests`（需传目录参数）在本机 8 轮 HDR 比对全挂（`SETTINGS_RESULT passed=0`）；
  已用 `git stash` 在**没有** DV 改动的基线上复测，同样 passed=0 → 与 DV 无关，非本功能回归

## 二、P7 双层合成算法

来源：**ETSI GS CCM 001 V1.1.1 (2017-02)** 第 5.4 节（公开规范，本地副本
`D:\DSH\.dsh-tmp\dovi-follow\etsi\gs_ccm001.pdf`，提取文本 `etsi\alg.txt`）。
FFmpeg 的 `AVDOVIRpuDataHeader` / `AVDOVIDataMapping` 已经把 RPU 解析成这些参数
（`coef_data_type` 注释："lavc always converts to fixed"）。

### 0. 输入

- BL：基础层图像，三个分量 `S0`（luma）/`S1`,`S2`（chroma），`BL_bit_depth` 位
- EL：增强层图像，同样三个分量，`EL_bit_depth` 位；EL 分辨率通常是 BL 的水平/垂直各一半
- RPU 参数（每帧）：`curves[3]`（piece-wise）、`nlq[3]`、`coef_log2_denom`、
  `bl_video_full_range_flag`、`disable_residual_flag`、`ext_mapping_idc_0_4`

### 1. BL 逆映射（5.4.2）

#### 1.1 pivot 索引（5.4.2.2）

对每个样本 `s`，找到 `pivot_value[cmp][i] <= s < pivot_value[cmp][i+1]` 的 `i`，
即 `pivot_idx`。`s` 超出范围时 clip 到首/末 pivot。

#### 1.2 多项式（luma，cmp=0）（5.4.2.3.2）

```
if (s <  pivot_value[cmp][0])                        s =  pivot_value[cmp][0];
if (s >  pivot_value[cmp][num_pivots_minus2[cmp]+1]) s =  pivot_value[cmp][num_pivots_minus2[cmp]+1];

ss = 1; shift = 20;                 // 2*(maximum BL_bit_depth)
vv = 0;
for (i = 0; i <= poly_order_minus1 + 1; i++) {
    vv += poly_coef[i] * (ss << shift);
    ss *= s;
    shift -= BL_bit_depth;
}
vv = (vv < 0) ? 0 : vv;
v  = vv >> (4 + coefficient_log2_denom);
v  = (v > 0xffff) ? 0xffff : v;
```

#### 1.3 MMR（chroma，cmp=1,2）（5.4.2.3.3）

需要**先把 BL luma 下采样到 chroma 分辨率**（边界按像素复制）。特征向量 `tt[]`
（20 位定点，order 1~3）：

```
tt[0] = 1<<20;
// order>=1
tt[1]=s0<<(20-BL_bit_depth); tt[2]=s1<<...; tt[3]=s2<<...;
tt[4]=(s0*s1)<<(20-2*BL_bit_depth); tt[5]=(s0*s2)<<...; tt[6]=(s1*s2)<<...;
tt[7]=(tt[4]*tt[3])>>20;
// order>=2
tt[8]=(s0*s0)<<(20-2*BL_bit_depth); tt[9]=(s1*s1)<<...; tt[10]=(s2*s2)<<...;
tt[11]=(tt[4]*tt[4])>>20; tt[12]=(tt[5]*tt[5])>>20; tt[13]=(tt[6]*tt[6])>>20;
tt[14]=(tt[7]*tt[7])>>20;
// order>=3
tt[15]=(tt[1]*tt[8])>>20; ... tt[21]=(tt[7]*tt[14])>>20;

rr = mmr_constant*tt[0];  cnt=1;
for (i=1; i<=MMR_order; i++)
    for (j=0; j<7; j++) { rr += mmr_coeff[i][j]*tt[cnt]; cnt++; }
rr = rr < 0 ? 0 : rr;
v  = rr >> (4 + coefficient_log2_denom);
v  = v > 0xffff ? 0xffff : v;
```

### 2. EL 逆量化 NLQ_LINEAR_DZ（5.4.3.2）

输出 `r` 是 **17 位有符号**：

```
T = fp_linear_deadzone_threshold[cmp];
S = fp_linear_deadzone_slope[cmp];
R = fp_hdr_in_max[cmp];

rr = e - nlq_offset[cmp];
if (rr == 0) { r = 0; }
else {
    sign = rr < 0 ? -1 : 1;
    rr <<= 1;  rr -= sign;  rr <<= (10 - EL_bit_depth);
    dq = rr * S;
    TT = (T << (10 - EL_bit_depth + 1)) * sign;
    dq += TT;
    RR = R << (10 - EL_bit_depth + 1);
    if (dq >  RR) dq =  RR;
    if (dq < -RR) dq = -RR;
    r = dq >> (coefficient_log2_denom - 5 - EL_bit_depth);
}
```

### 3. HDR 信号重建（5.4.3.3）

`out_bit_depth`：BL 的传输特性是 BT.1886 → 14；否则（ST 2084）→ `hdr_bit_depth`
（典型 12）。**我们的 P7 片源 BL 是 PQ，所以走 12 位分支。**

```
MAXOUT = (1 << out_bit_depth) - 1;
h = v;
if (!disable_residual_flag) h += r;
h += (1 << (15 - out_bit_depth));
h >>= (16 - out_bit_depth);
h = h < 0 ? 0 : h;
h = h > MAXOUT ? MAXOUT : h;
```

### 4. 转换到 ST 2084（5.5）

**BL 已经是 ST 2084 时整段跳过**（规范原文："this conversion process shall be
omitted"）。我们的 P7/HDR10 片源正好属于这种情况 —— 省掉 RGB 往返与 BT.1886 反
EOTF，是这条链路最省的一处。

### 5. 对我们的实现含义

- 每帧需要的参数全部可从 FFmpeg 的 `AVDOVIMetadata` 取：
  `rpu_data_header`（bit depth / coef_log2_denom / disable_residual_flag / ext_mapping_idc）
  + `vdr_dm_data.mapping`（curves[3] / nlq[3]）
- 多项式与 MMR 都是 20 位定点，**适合直接搬进 compute shader**（int64 需降到 32 位或
  用双 32 位模拟；`tt[]` 的 20 位定点乘法要防溢出）
- luma 走多项式、chroma 走 MMR 且需要下采样 luma —— 意味着合成 pass 不是逐像素独立，
  chroma 需要 luma 的 2×2 邻域
- EL 是 1080p、BL 是 4K：EL 需要 2× 上采样，且 Dolby 的 EL 是"半分辨率残差"

### 6. 未取到的细节（下一步要补）

- `pivot_value[]` 的具体推导（pivot 编码位宽与对齐），第 17 页开头提到但提取不全
- `fp_hdr_in_max` 与 EL 的对应关系（在 5.3.4 的语法定义里）
- `ext_mapping_idc` 的非零分支（扩展映射）是否出现在我们的样本里

（2026-10-17 更新：`pivot_value` 在本样本里就是 RPU 的 `pivots[]` 原值，见下节实测。）

## 三、代码位置索引

| 功能 | 文件 |
|---|---|
| P5 转换 | `include/veyra/pipeline/DolbyVisionP5.h` |
| P7 增强层 | `include/veyra/pipeline/DolbyVisionP7.h` |
| BL+EL 合成 | `include/veyra/engine/DoviBlElCompose.h` |
| 曲线适配（L1 跟随）| `include/veyra/engine/DoviCurveAdapter.h` |
| 源侧路由 | `include/veyra/source/DolbyVision.h` |
| 色彩转换 shader | `shaders/YuvToLinearRgb.hlsl` |
| 验证工具 | `tools/dovi_p5_probe/` |

**当前路由行为**（`src/source/MediaFileSource.cpp`）：

```
[source-dovi] profile=8 level=7 compatibility=4 bl=true el=false rpuDeclared=true route=4 nativeOutput=0 rpuApplied=0
```

- `route=4` = HLG 基础层（P8 + compatibility=4 时）
- `nativeOutput=0` / `rpuApplied=0` = 不输出原生 DV、不应用 RPU
- 这是作者既有设计，P8 按 HLG HDR 播放

---

---

## 四、HDR10+ 动态元数据

HDR10+ 是三条动态元数据路线（杜比视界 / HDR10+ / HDR Vivid）里**唯一有公开标准、也有开源参考实现**的一条：它就是 **SMPTE ST 2094-40**，libplacebo 里叫 `st2094-40`。

### 做了什么

#### 读取

源层读每帧的 `AV_FRAME_DATA_DYNAMIC_HDR_PLUS`，取第一个处理窗口：

```cpp
const auto& win = plus.params[0];
peakLinear = max(maxscl[0], maxscl[1], maxscl[2]);   // 线性，0..1
avgLinear  = average_maxrgb;                          // 线性，0..1
```

两个字段都是**线性亮度、归一化到 10000 nits**（FFmpeg 头文件里写明），所以：

```cpp
maxPq = nitsToPq(peakLinear * 10000.0);
avgPq = nitsToPq(avgLinear  * 10000.0);
```

PQ 转换放在 `pipeline/PqTransfer.h`，和 `pqToNits` 同一处——源层用正向、曲线适配器用反向，分成两份早晚会漂移。

#### 传输

结果装进 `DoviFrameInfo`（`valid / profile / minPq / maxPq / avgPq`），**`profile` 填哨兵值 `kHdr10PlusProfile = 100`** 标明来源。杜比的 profile 只到 10，不冲突。

这样不用在整条链路上多传一个结构体：跟随逻辑要的只是"一个峰值 + 一个平均值"，和杜比 L1 的形状完全一样。

**互斥保证**：`if (!out.dovi.valid)` —— 有杜比 RPU 时绝不覆盖。

#### 应用

复用 `engine::doviCurveFromLevel1`：移动 HDR 曲线的滚降起点，其它参数（暗部提升、中灰、滚降强度）保持用户设定。

生效条件仍然是那两个开关：

```
HDR 亮度曲线（hdrCurve） + 跟随动态元数据（doviFollow）
```

日志现在会标来源：

```
dovi-follow n=200 follow=1 applied=1 source=HDR10+ peakNits=980.5 highlightStartNits=833
dovi-follow n=200 follow=1 applied=1 source=DV7     peakNits=442.8 highlightStartNits=376
```

### 三个 bug（都只在 `applied=1` 时才看得见）

这三个都是"开关打开、真正生效"之后才暴露的，之前只验证数据流时全部看不出来——**教训是：验证一个跟随功能，必须让它真的作用于画面**。

#### ① 滑动平均的算术 bug

原来用 12 帧滑动窗口平滑峰值，溢出时做：

```cpp
peakSum_ *= 0.5;  peakSumCount_ /= 2;   // 25/2 = 12，丢掉 0.5
```

**奇数计数时比值被放大**，而且每次溢出都再放大一点、永不回落。实测：

```
maxPq=1.0000  smoothedMaxPq=1.1111  →  peakNits=52469
```

平均值比它**所有输入的最大值还大**。改成 EMA（指数平滑）后无累积误差。

同样的 bug 在平均亮度的窗口里也有（`avgSum_`），只是数值小没暴露。

#### ② 跟随了不可能的元数据

测试片里有 `maxscl = 1.0` 的帧（**10000 nits**）。没有电影母版是 10000 nits。代码照样跟 → 膝盖顶到上限，下一帧掉回来 → 就是闪。

现在这类帧**直接忽略**，保持上一个合理值。

#### ③ `AVRational` 除零

`maxscl` / `average_maxrgb` 是 `AVRational`，`av_q2d` 直接算 `num/den`。**这些流里存在 `{num, 0}`**，除零得到 `+inf`，经 `nitsToPq` 被 clamp 成 1.0 PQ（=10000 nits）。

表现是：每 10 秒出现一个假的"10000 nits 帧"。

修复：用带 `den != 0` 检查的 `ratio()` 替代 `av_q2d`。

## 五、HDR Vivid 与 Profile 8.4

> 本文以 HDR Vivid 命名，实际覆盖同一天做的四件事，查 P8.4 或格式标签也从这里找：
>
> 1. HDR Vivid 动态元数据识别
> 2. 统一格式标签 + 起播徽章
> 3. Profile 8.4（HLG 基底）的识别与色彩路由
> 4. 「图形字幕 HDR 色彩分离」为什么在 Veyra 不适用

### 为什么做

mpv-Yaozhi 的宣传点之一是「HDR Vivid 元数据识别」，Veyra 此前把这类片源**当成普通 HDR10**
处理——画面和日志都看不出区别。本轮只做识别，不做 Vivid 的动态色调映射。

### FFmpeg 已经解析好了

FFmpeg n9.0.1 内置 CUVA 005.1:2021 支持，不需要自己解 SEI：

- `libavutil/hdr_dynamic_vivid_metadata.h`：`AVDynamicHDRVivid`、`AVHDRVividColorTransformParams`
- `libavutil/frame.h:211`：`AV_FRAME_DATA_DYNAMIC_HDR_VIVID`

HEVC 解析器把每个 SEI 挂到对应帧的 side data 上，取用只有一次 `av_frame_get_side_data`。

### 实现

| 文件 | 改动 |
|---|---|
| `include/veyra/source/IFrameSource.h` | 新增 `HdrVividInfo`（present / startCode / numWindows / framesWithMetadata），挂到 `SourceInfo` |
| `src/source/MediaFileSource.cpp` | `readBase()` 里检测 side data：首帧打印完整参数，之后每 600 帧打印一次窗口亮度 |
| `src/engine/EngineController.cpp` | `snapshot_.colorStatus` 的输入侧在 Vivid 时写 `HDR Vivid` 而不是 `HDR`，播放条和信息对话框直接受益 |

关键点：**亮度字段是 `AVRational`，分母可能为 0**，取值前必须判断，否则得到 inf/nan。

### 实测（2160p60 HDR Vivid WEB-DL，3850×1608，2.7 GB）

```
[source-hdr] HDR Vivid dynamic metadata observed startCode=1 windows=1
             minMaxRgb=0.0330 averageMaxRgb=0.0354 maxMaxRgb=0.0381
             toneMapping=0 toneMapParams=0 saturation=0 satParams=0
[source-hdr] HDR Vivid frames=600  window0 minMaxRgb=0.0283 averageMaxRgb=0.2703 maxMaxRgb=0.6212
[source-hdr] HDR Vivid frames=1200 window0 minMaxRgb=0.0357 averageMaxRgb=0.5368 maxMaxRgb=0.7258
[source-hdr] HDR Vivid frames=1800 window0 minMaxRgb=0.0274 averageMaxRgb=0.5006 maxMaxRgb=0.6867
[source-hdr] HDR Vivid frames=2400 window0 minMaxRgb=0.0388 averageMaxRgb=0.4730 maxMaxRgb=0.6261
```

`averageMaxRgb` 随画面在 0.27 / 0.54 / 0.50 / 0.47 之间跳动——**这就是"动态"的证据**，
静态标记不会这样变。首帧值很小是因为片头是黑场。

界面：播放条读 `2160p60 · HDR Vivid`；同一份代码遇到 Profile 7 FEL 测试片仍读 `HDR`，
没有误标。

### 起播格式标签

同一天还补了 mpv-Yaozhi 的另一个功能：一个统一的格式名 + 起播徽章。

- `PlayerSnapshot::sourceHdrLabel` 给出一句话的格式名：`Dolby Vision P5/P7/P8`、
  `HDR Vivid`、`HDR10+`、`HDR10`、`HLG`、`SDR`。播放条和信息对话框都读它，
  所以以前那句笼统的 `HDR` 现在会说清是哪种。
- 起播徽章画在画面左上角的安全区。**视频是原生子窗口，QML 浮层盖不上去**，
  所以用了和弹幕/字幕一样的构造：`WS_EX_LAYERED | WS_EX_TRANSPARENT` 子窗口 +
  `UpdateLayeredWindow`（`src/media/FormatBadgeWindow.cpp`）。画一次，3.2 秒后隐藏，
  没有逐帧开销。
- 触发条件是「Playing **且** 已经处理过第一帧」。只判断 Playing 会在源刚打开时就触发，
  那时第一帧的动态元数据还没到，Vivid 片源会被标成 `SDR`——这是实测踩到的。

实测：`[format-badge] shown title=HDR Vivid detail=1608p60 · 3840×1608`，
同时播放条读 `1608p60 · HDR Vivid`。

### Profile 8.4（HLG 基底）

P8.4 = Profile 8 + compatibility id 4，基底是 **HLG 编码**（8.1 是 PQ）。

`DolbyVisionInfo::route()` 本来就已经把 `compatibility==4` 映射到 `DolbyBaseLayer::Hlg`，
问题出在 `open()` 的色彩钉死只处理了 `Hdr10` 分支：基底 VUI 没标传输函数时，
`transfer` 停在 Unknown，最后落到 SDR 路径——传输函数错两次。

修法：`Hdr10` 和 `Hlg` 两个路由都钉死（矩阵/原色仍是 BT.2020），并在日志里写清钉到了哪一个。
8.4 的片源现在不可能被钉成 PQ。

格式标签也细分了：`Dolby Vision P8.1` / `P8.2` / `P8.4`，徽章和播放条都会显示具体变体。

**RPU 仍然不应用**——兼容播放本来就不应用 DV 动态元数据，所以 8.4 显示的是它的 HLG 基底，
和 8.1 显示 PQ 基底是同一件事。

#### 实测（B 站「Galaxy」官方演示片，3840×2160 60fps，109 秒）

片源来自 B 站杜比视界清晰度档（画质 id 126），流下来的 DV 配置就是 P8.4：

```
[bili-quality] cid=41859091139 ids=[126 120 116 80 64 32 16]
[source-dovi] profile=8 level=9 compatibility=4 bl=true el=false rpuDeclared=true route=4
[source-dovi] base layer pinned to HLG (profile 8.4 is an HLG base, 8.1 is PQ)
[source-file] opened 3840x2160 dur=109.23s avgFps=60.000 ... transfer=6
[hdr-route] input=HLG ... decode=inverse-OETF+OOTF-gamma1.2 ... output=PQ-BT2020-RGB10
[format-badge] shown title=Dolby Vision P8.4 detail=2160p60 · 3840×2160
```

关键是从 `compatibility=4` 到 `route=4`（Hlg）再到 `input=HLG` +
`decode=inverse-OETF+OOTF-gamma1.2`（HLG 专用解码，不是 PQ 的 ST2084）整条链是通的。
画面正常：如果基底被钉成 PQ，这一段逆光森林会明显发灰。

**顺带纠正一个判断**：B 站并不是"一律剥掉 DV"。列表里预加载的另一个视频只有
`ids=[125 …]`（最高 HDR），而点进 Galaxy 后拿到的是 `ids=[126 …]`——126 就是杜比视界档，
而且 B 站的 DV 转码正是 P8.4（HLG 基底）。所以 B 站反而是最省事的 P8.4 测试源。

### 明确没做的

1. **动态色调映射**：Vivid 的 `tone_mapping_param_num` / 三样条曲线没有参与映射，色彩链
   仍走静态曲线。调研过，结论是优先级很低，理由见下。
2. **处理窗口**：`num_windows` 读到并记录了，但只读 `params[0]`；本片源就是 1 个窗口。

#### 为什么 Vivid 动态映射没有现成方案可抄

| 项目 | 对 Vivid 做了什么 |
|---|---|
| FFmpeg | **只解析**（`AVDynamicHDRVividMetadata`），不应用 |
| libplacebo / mpv | 完全不支持 Vivid |
| VLC | 没有 Vivid 的 tone mapping |
| CUVA 官方 | 有「编码侧参考代码」（如何**生成**元数据），不在开源仓库 |

**应用层是空白。** FFmpeg 里的 `AVHDRVividColorTransformParams` 只是结构定义，
不带算法。

#### 更关键：片源不一定带可用的参数

本片源实测：

```
windows=1 minMaxRgb=0.0330 averageMaxRgb=0.0354 maxMaxRgb=0.0381
toneMapping=0 toneMapParams=0 saturation=0 satParams=0
```

`toneMapping=0` 即 `tone_mapping_param_num = 0`——这个场景**没有携带任何 tone
mapping 曲线**，只有亮度统计。

#### 和杜比视界的本质区别

| | DV RPU | HDR Vivid |
|---|---|---|
| 元数据类型 | **指令**：明确这一帧怎么映射 | **参考**：给亮度统计，映射方式由播放器决定 |
| 播放器要做什么 | 按 RPU 执行 | 自己判断要不要用、怎么用 |

所以 Vivid 的「动态映射」没有标准答案，各厂商（海信/TCL/华为）调法不同，
这也是没有统一开源实现的原因。

**真要做的唯一有依据的做法**：用 Vivid 的亮度统计（`averageMaxRgb` /
`maxMaxRgb`）驱动已有的 HDR 曲线，复刻 DV 的 L1 跟随，只是换数据源。改动小，
不依赖 tone mapping 参数是否存在，但效果同样只是近似。

### 未实测的功能（代码在，但没跑过真实数据）

这两条容易在交接时被误读成"已验证"，单独列出来：

| 功能 | 实现位置 | 缺什么 |
|---|---|---|
| **HDR10+ 识别** | `MediaFileSource::readBase`（`AV_FRAME_DATA_DYNAMIC_HDR_PLUS`） | 一个 HDR10+ 片源都没有 |
| **MEL 跳过** | `readBase` 的 `elMelStrikes_` 判定 + `read()` 关闭 EL | 没有 MEL 片源（手上 P7 全是 FEL，`disable_residual_flag=0`） |

两条都只有代码和逻辑，没有实测证据。真要用之前先拿片源验一次。

### 相关结论在别处

P7 的多段曲线取舍、L5 有效区域的处理决定，写在 本文第一节的
「多段曲线：量完之后决定不做」和「L5 有效区域：查到了，但不动」两节——那里有实测数据。

### 不适用（架构决定）：图形字幕 HDR 色彩分离

mpv-Yaozhi 维护了一个核心补丁，让 UHD HDR/DV 片源里的 PGS 图形字幕能"随视频进入 HDR
映射链"，同时给外置/SDR 字幕单独的色彩与 150–400 nits 亮度档位。

**Veyra 不需要这个功能，而且做了也没有意义。** 原因是两边字幕的合成位置不同：

| | mpv-Yaozhi | Veyra |
|---|---|---|
| 字幕在哪合成 | 视频渲染管线内（`gpu-next`） | **独立的原生子窗口** |
| 怎么画 | 图元合成进 GPU 输出 | GDI+ 画 32 位 DIB → `UpdateLayeredWindow` |
| 色彩空间 | 继承视频，会被传递函数/色调映射连带处理 | **永远是 sRGB**，从不进入视频色彩链 |

也就是说，那个补丁要修的"字幕被 HDR 链错误染色"在 Veyra 里**不可能发生**：
`SubtitleView`（`apps/veyra/ui/SubtitleOverlay.h`）只有
`scale / outline / background / bottomMargin / blockGap / targetLines / fitToLines`，
`engine::SubtitleStyle` 里也只有 ASS 定义的 ARGB 颜色——**没有任何和传输函数、
nits、色彩空间有关的参数**，因为它根本不参与色彩运算。

HDR 播放时唯一能观察到的现象是：sRGB 白字幕的亮度由 Windows 的"**SDR 内容亮度**"
决定（通常 200–300 nits），可能比画面显得偏亮或偏暗。那是 SDR 叠加层的正常表现。

如果以后真觉得刺眼，正确形态是一个「字幕亮度/不透明度」偏好（默认 100%，在
`updateSubtitleOverlay` 里对 DIB 做一次 alpha 缩放），属于舒适度调节，不是功能补齐。
**不要**把字幕并进视频 HDR 链：要重写字幕合成、libass 路径和位图字幕三条路，风险高
且没有任何收益。

### 后续待办

- 双轨 P7（BL/EL 分成两条 track）：需要片源，比较少见
- Vivid / HDR10+ 的动态色调映射：工程量大，且 mpv-Yaozhi 同样只做识别

## 附、已归档的验证记录

以下内容原在本文件，属于一次性验证证据，已归档（git 历史可查）：

- **Profile 5 端到端验证**（2026-10-17）：16 个样本、硬软解对比、参考解码器对拍。
  结论：P5 转换各帧参数自洽、不依赖播放历史、HDR10/SDR 两条输出路径均正确。
- **P7 原型实测**（2026-10-17）：EL 覆盖率 51%~97%、性能回落阈值、画面验证结果。
  结论：P7 双层合成在当前实现下可行，EL 缺失时回落到 BL。
- **HDR10+ 验证**（2026-10-29）：ff.de 测试片、三个 bug 的复现数据。
  结论：三个 bug 只在 applied=1 时可见；读传通道已修好。

需要原始数据时：
