# 历史技术记录

> 从 `CUSTOM_LEDGER.md` 拆出。按日期倒序（最新在上）；末尾是早期按主题编号的旧结构。


> **文档地图**（我自己的文档，作者原版 600+ 份在 `docs/` 不动）
>
> | 文件 | 一句话定位 | 什么时候看 |
> |---|---|---|
> | **MY_WORK.md** | 项目现状、定制功能、行动规则 | 新会话第一步 |
> | **HANDOFF.md** | 构建、部署、验证手册 | 要构建或部署时 |
> | **docs/DOLBY_VISION.md** | 杜比视界 P5/P7/L1 技术档案 | 改杜比视界相关代码 |
> | **docs/HISTORY.md** | 技术排查链（按日期） | 遇到具体技术障碍 |


## 📌 读前必看：本文的路径全部是历史记录

**文中出现的 `<旧项目目录>\...`、`build-nv`、`deps\...` 等路径是当时的真实记录，
原样保留不改。** 那个目录已于 **2026-10-25 删除**，现在全部迁到 `<项目目录>\`：

| 文中的旧路径 | 现在的位置 |
|---|---|
| `<旧项目目录>\Veyra-NRVideo` | `<项目目录>\src` |
| `<旧项目目录>\build-nv` | `<项目目录>\build` |
| `<旧项目目录>\deps\ffmpeg\...` | `<项目目录>\deps\ffmpeg\...` |
| `<旧项目目录>\Veyra-2.0.x-...portable` | `<项目目录>-2.1.0-NVIDIA-win64-portable` |

**照抄文里的命令之前，先把路径换成上表的新位置。**

原因是**中文路径会让 CMake 的 Ninja 生成器写不出 `rules.ninja`**，构建直接失败——
这也是当初搬迁的唯一原因。当前可用的构建命令见本地工作笔记（不在本仓库）。

---

## 一、按日期（最新在前）

### ✅ 2026-11-02 自动倍率打开就报错、重启后还丢失：三层叠加

**现象**：打开「自动选择倍率」，0.3 秒后日志出现
`[ERROR] [ui-session] 模式会话编码校验失败，未保存`；重启后开关又变回关闭。

**第一层：会话版本 ↔ 库版本的映射没跟上**

会话存档里嵌着预设条目，读取时按「库版本」解析，而那张映射表写死到 16：

```cpp
version>=10 ? 16 : version>=9 ? 14 : version>=8 ? 13 : 11
```

自动倍率标记需要库 **v18**（作者的 v16 三步超分、v17 双重超分已占用，本地功能顺延），
于是 v13 会话被当成 v16 解析，字段整体错位，`decode` 校验不过。
修法是把映射补成 `>=13→18 / >=12→17`。

**第二层：只改了一条链，保存被"没变化"短路**

`setFgAutoMultiplier` 只写 `impl_->chain`，而 `currentSession()` 取的是
`activeRuntime()` —— 节点模式返回的是 `acceptedChain`。两者比较相等，
`persistSession()` 走提前返回，一个字节都没落盘。

**第三层：回滚也只回滚一条链**，同样的疏漏。

**验证**（`--fg-auto on` / 重启 / `off`）：

| 步骤 | `chain-session.v1` 头部 |
|---|---|
| 打开 | `VEYRA_CHAIN_SESSION 13` |
| 重启 | 仍是 13，无编码错误 |
| 关掉 | 退回 `VEYRA_CHAIN_SESSION 1` |

**教训**：会话存档是「写一份、立刻读回来验一遍」的设计，所以任何一端漏字段都会立刻
暴露成"保存失败"，而不是悄悄存坏。这类改动必须同时改 encode、decode 的版本映射、
以及两条链的写入——少一处就整条链失效。


### ✅ 2026-11-02 合并后弹幕一条都画不出来：tickDanmaku 整段丢失

功能链路复检时发现：`bilibiliRequestEnableDanmaku` 正常建窗口、正常拉数据、
`setItems` 也塞满了，但**没有任何东西驱动 `render()`**。

`grep -rn 'danmakuWindow->render|->follow(|->noteSeek(' src/` 零命中——2.1.2 把
`QmlPlayerBridge.cpp` 拆成 `src/ui/bridge/*.cpp` 时，`tickDanmaku()` 的函数体、头文件
声明、以及主循环里的调用**三样一起丢了**。`danmakuLastSeconds` 成了死字段，
`BilibiliPage.qml` 里四个弹幕外观滑块也失去了 C++ 消费者。

恢复方式：从合并前的提交取函数体放进 `Bilibili.cpp`，声明放回 `tickSubtitles()` 旁边，
调用接在 `tickSubtitles()` 末尾——弹幕本来就是跟字幕共用同一次每帧 tick，暂停、跳转、
变速都不需要额外接线。

**教训**：文件拆分时，一个函数可能同时有「定义、声明、调用点」三处，少任何一处都不会
报编译错误（未声明的成员函数调不到，但没人调就不会有人调它）。这类"功能整体消失但
编译干净"的缺口，只能靠逐条链路追代码路径发现，编译和单元测试都盖不住。


### ✅ 2026-11-02 合并到 2.1.2 之后，十三个样本重测格式识别

**为什么测**：这次合并把桥接层整个搬进了 2.1.2 的拆分架构，杜比视界、HDR10+、Vivid 的
识别路径都在被搬动的代码里。编译通过和单元测试全绿只能证明算数对，不能证明"打开的
片子还是被认成原来那个格式"，所以拿真实文件重跑了一遍。

**十三个样本，分两组**

杜比视界（6 个，详见 `docs/DOLBY_VISION.md` §11）：

| 样本 | 配置 | 识别结果 |
|---|---|---|
| FEL RPU_EL测试 / FEL TEST ST DL P7 CMV4.0 / [Dolby] … EL Test | P7 + FEL | `profile=7 el=true route=2` |
| Despicable Me … Dolby Vision | P7 MEL | 同上，走内置 NAL 遍历 |
| Spider-Man.Brand.New.Day … DV.HDR | P8 | `profile=8 compatibility=1 el=false` |
| 杜比视界P10_AV1_1080p | P10 (AV1) | `profile=10 level=4`，RPU 读到但不应用 |

非杜比（7 个）：

| 样本 | 实际 | 识别结果 | 判据 |
|---|---|---|---|
| hdr10+test_lake_2021 | HDR10+ | `HDR10+ scene metadata peak=395.6 nits` | 场景元数据真的解出来了 |
| S01E01 … HDR Vivid | Vivid | `HDR Vivid dynamic metadata observed windows=1` | 同上 |
| Spider-Man No Way Home / Taylor Swift | HDR10 | `masteringMaxNits=1000` + `maxCLL` | 静态元数据 |
| [Dolby] … Layer Control | **HDR10** | 一行 `[source-dovi]` 都没有 | **文件名骗人**：名字全是 Dolby Vision，实际没有 DV 记录 |
| TS演唱会 / 2025.iTALiAN | SDR | `hdr=false transfer=2` | 没被误判成 HDR |

**结论**：13/13 全部 `exit=0`、0 条 ERROR，识别与合并前一致。两个容易出错的点都守住了——
「文件名带 Dolby 但无 DV 配置记录」不再被当 DV，「SDR 片源」不再被当 HDR。

**唯一的异常**：P10 那个 AV1 10bit 样本走 D3D12VA 失败后回退 libdav1d 软解
（`unsupported-d3d12-surface`）。1080p 软解无压力，4K AV1 10bit 可能吃力；
这是驱动/FFmpeg 的 AV1 硬解支持问题，不是本链引入的。


### ✅ 2026-10-17 AMD 光流（FidelityFX）从"根本编不进来"到编译通过：一条完整的排查链

### 现象
在「DLSS 帧生成 + 6X」下，一旦把「光流算法」选成 **AMD FidelityFX**，顶部补帧开关立刻被关掉。
作者版（同一 2.0.9 源码）却能开。日志里（`logs\veyra-qml.log`）一句话点明：

```
[ERROR] [amd-of] FidelityFX SDK not compiled into this build
[WARN ] [backend-recovery] initialization failed component=2 -> nr=false sr=false multiplier=1
[ui-fg-rollback] rejectedRevision=9 restoredBackend=0 restoredMultiplier=1
```

### 根因
`src/guidance/AmdOpticalFlow.cpp` 用 `#ifdef VEYRA_HAS_AMD_OF` 包裹整段实现；该宏由
`CMakeLists.txt` **L1310** 的条件定义：只有
`${VEYRA_FIDELITYFX_ROOT}/bin/ffx_sdk/ffx_opticalflow_x64.lib` **存在**才定义，
并链接 `ffx_opticalflow_x64.lib` + `ffx_backend_dx12_x64.lib`。
而我们本机没有这套库 → 宏没定义 → initialize() 直接走 `#else` 返回 false → 引擎拒绝提交 → 设置回退 → 补帧被关。

### 关键结论（最省时间的一条）
**官方三个发行包里都没有 `ffx_opticalflow_x64.lib`**：
* FidelityFX SDK **1.1.4**（449 MB）：只有 `ffx-api/bin/amd_fidelityfx_dx12.lib`（新 API），无经典光流库
* FidelityFX SDK **1.1.0**（331 MB）：同上
* FidelityFX SDK **1.0**（388 MB）：`bin/ffx_sdk/` 有 16 个库（fsr1/fsr2/cas/dof/denoiser…）但**没有 opticalflow**
→ 它**只能从源码编**。**别再为这个库去下发行包了**（本次白下了 1.17 GB）。

### 从源码编出来的完整步骤（可复用）
1. 解 1.1.4 发行包里的 `sdk/` 源码（864 个文件）；顶层 CMake **本来就有** `add_subdirectory(${FFX_COMPONENTS_PATH}/opticalflow)`（L235），
   且有 `option(FFX_OF "Build FFX Optical Flow API" ON)`。**不要自己在末尾再 add_subdirectory**（会让 configure 直接失败 ✗，本次踩过）。
2. SDK 的 CMake 里把 `/WX`（警告当错误）写死（`src/backends/dx12/CMakeLists.txt`、`src/components/fsr1|fsr2`），
   `-DCMAKE_CXX_FLAGS=/WX-` 覆盖不掉 → 直接把副本里的 `/WX` 改成 `/WX-`。
3. **CRT 必须一致**：本仓库是静态 CRT（/MT），SDK 默认 /MD → 链接报
   `LNK2038: RuntimeLibrary 不匹配：MD_DynamicRelease vs MT_StaticRelease`。
   修法：`cmake -S <sdk> -B <新目录> -G "Visual Studio 17 2022" -A x64 -DFFX_API_BACKEND=DX12_X64 -DFFX_OF=ON -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`
   （`-DCMAKE_CXX_FLAGS=/MT` 之类不生效，必须用 `CMAKE_MSVC_RUNTIME_LIBRARY`）。
   验证：`dumpbin /directives xxx.lib | findstr DEFAULTLIB` 应看到 `libcpmt`/`LIBCMT`。
4. 编 `ffx_opticalflow_x64` 与 `ffx_backend_dx12_x64` 两个目标。
5. **产物不在构建目录**！SDK 把 `CMAKE_ARCHIVE_OUTPUT_DIRECTORY` 指回 **源码树**：
   `<sdk>/bin/ffx_sdk/*.lib`（这次就是因为在构建目录里找，白折腾了两轮 ✗）。
6. 把两个 `.lib` + 1.1.4 的 `sdk/include` 放到 `third_party_local/amd/FidelityFX-SDK/sdk/` 下的
   `bin/ffx_sdk/` 与 `include/` → 重新 cmake → `build.ninja` 里应出现 `VEYRA_HAS_AMD_OF`（本次 17 处）。

### 结果
整树编译 rc=0；exe 从 `C7E2693A…` 变为 **`74D1F7CB…`**；单元测试全绿（2055 项 0 失败）；
已部署（生效 exe = build = `.p5` = `74D1F7CB…`，作者版 `.orig` 仍 `7ADD56B9…`），启动退出码 0。

### 若以后还要重做（或在别的机器上）
只需要三步：**源码编两个 /MT 库 → 放进 `third_party_local/amd/FidelityFX-SDK/sdk/{bin/ffx_sdk,include}` → 重新 cmake + 编**。
源码构建目录可留在 `<本地笔记>\ffx114\sdkbuild\`（用完可删，库已进仓库）。

### ✅ 2026-10-17 光流算法：NVIDIA NVOF 与 AMD FidelityFX 双开，并实测 AMD 跑通

### 需求
「光流算法」选择器里 NVIDIA 与 AMD 两个都要能用。

### 做法
1. **NVIDIA NVOF**：SDK 本来就在（`third_party_local/nvidia/Optical_Flow_SDK_5.0.7`），
   只是开关是 OFF。重新配置 `-DVEYRA_ENABLE_NVOF=ON` 即可（零下载）。
2. **AMD FidelityFX**：本机缺 SDK（`third_party_local/amd` 不存在），
   用 **浅克隆 + blob 过滤 + 稀疏检出** 只取 `Kits/FidelityFX`：
   ```
   git clone --depth 1 --filter=blob:none --sparse --branch v2.3.0 \
     https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git <repo>/third_party_local/amd/FidelityFX-SDK-2.3.0
   git -C <该目录> sparse-checkout set Kits/FidelityFX
   ```
   **实测占用 154.6 MB**（比官方 minimal 包 ~125 MiB 的整包略大，因为 Kits 里含预编译库；但**不拉 Samples**）。
   放好之后 **不需要任何开关** —— CMake 里是 `if(EXISTS …/ffx_api_loader.h)` 守卫，`VEYRA_HAS_FSR` 会自动定义
   （`build.ninja` 里从 0 处变成 9 处，这就是"生效"的铁证）。

### 结果
* 整树编译 rc=0（147 步），测试全绿（hdr_tuning 214/0、preset_library 505/0、ui_i18n 0 failures、
  effect_chain 319/0、repair_contract 272/0、vfg_settings 683/0、lmxxf_nr 62/0）。
* 部署：生效 exe = build = `.p5`（`C7E2693A…`），`.orig` 仍是作者版（`7ADD56B9…`）；启动退出码 0，无残留进程。

### FSR 集成测试的运行姿势（重要，不然会误判）
* `veyra_fsr_dispatch_tests`：参数可选，传 `amd` 走 AMD 后端。
* `veyra_fsr_switch_tests`：**必须给一个输出目录**（`argc<2` 直接 `return 2`），第二个参数可选 `amd`。
* 两者都要求 **`runtime_local` 在 exe 旁边**（这是**跟 exe 走**的，不是跟工作目录！）。把包里的
  `runtime_local/amd/fidelityfx/*.dll` 复制到 `build-nv/runtime_local/amd/` 后：
  * `FSR_INDEPENDENT pass=1 samples=39 intermediate=39 meanError=0` ✓（默认与 `amd` 参数都通过）
  * `SWITCH_CASE cycle=1 backend=AMD-FSR multiplier=2 generated=38 pass=1` ✓
  → **AMD FidelityFX 光流在实机上确实跑通**，不是"编进去就算"。

### XeSS（第三个后端）现状：不可用
* 程序找 `runtime_local/intel/experimental/libxess_fg.dll`，而包里/`runtime_local/intel/` 下的
  `libxess_fg.dll` 是 **0 字节占位**；`third_party_local/intel` 根本不存在 →
  `SWITCH_CASE cycle=2 backend=XeSS pass=0`。
* 要启用需要真正的 Intel XeSS 运行时（SDK 或作者 runtime 包），本次不做。**与 NVIDIA/AMD 两个无关。**

### ✅ 2026-10-17 补记：让整树编译在 2.0.9 上全绿（两处 CMake 适配）

升级验证时只编了"主程序 + 8 个测试目标"（33 步）就通过了，但**整树编译（424 步）**在 2.0.9 上还需要两处适配：

1. **FSR 诊断探针缺 AMD SDK** —— `tools/fsr_probe/main.cpp`（69 处）与 `tools/fsr_upscale_probe/main.cpp`（35 处）
   直接 `#include` FidelityFX SDK 头，而上游是**无条件** `add_executable` 它们。
   本机 `third_party_local` 只有 `nvidia`（857 MB），没有 `amd`。
   **修法**：照 `veyra_gfx` 已有的守卫（`if(EXISTS …/ffx_api_framegeneration_dx12.h)`），
   把两支探针包进 `if(EXISTS "${VEYRA_FIDELITYFX2_ROOT}/api/include/ffx_api_loader.h")`。
   探针是**诊断工具**，播放器与测试都不需要它们；装了 SDK 就会自动参与编译。
   顺带核实：`VEYRA_FIDELITYFX_ROOT`（1.1.4）在 CMakeLists 里只见定义、没有使用点，**不需要**。

2. **一批测试目标漏 FFmpeg 头路径** —— `veyra_audio_timeline_tests`、`veyra_capture_audio_tests`、
   `veyra_audio_playback_rate_tests` 等编译时报 `NetworkInput.h(9): 找不到 libavformat/avformat.h`。
   上游依赖自己的构建环境把这些路径导出，本机没有。
   **修法**：在测试目标之前加一条全局 `include_directories(${FFMPEG_INCLUDE_DIRS})`（比逐个目标打补丁更彻底）。

### 结果
整树编译 **424 步、rc=0**，产出 **116 个测试可执行文件**。定制 exe 同步进程序目录并刷新 `.p5`，
作者版 `.orig` 保持不变；启动一次 **退出码 0** ✓。

### 依赖结论（供以后判断要不要装 AMD SDK）
* 播放器与全部测试**都不需要** FidelityFX SDK：主程序里 FSR 后台本来就有守卫（`VEYRA_HAS_FSR` 未定义即关闭）。
* 只有两支诊断探针需要；官方 2.3.0 release 的 "Minimal package" 标注 **~125 MiB**，
  我们真正需要的只是 `Kits/FidelityFX` 下的 `api/include`、`framegeneration/include`、`upscalers/include`（远小于 125 MiB，未实测）。
* 结论：**不装 SDK 也能让整树全绿** —— 已用上面的守卫实现。

### ✅ 2026-10-17 杜比视界 P3-2 / P3-3：逐帧 L1 接进呈现曲线，并做成真开关

### 做了什么
1. **P3-2a..d（e1d39c93）**：`VideoPresenter` 新增 `latestDovi_` + `setFrameDovi()`；
   `EngineController` 在帧处理完后把 `pkt.dovi` 交过去（暂停重绘路径也交）；
   `present()` 里用 `doviCurveFromLevel1` 得到 effective 曲线，**只替换 dims[8..13]**，
   `probeCurve_` / 显示端元数据仍用静态曲线；保留帧复用比较改用 effective 曲线。
2. **P3-3**：`EnhancementSettings::doviFollow`（live）+ `describeStages()` 写入
   `EnhanceGraphDesc::doviFollow` + `applySettings` 里 off→on/on→off 打一行 `hdr-dovi`；
   UI 在「杜比视界设置」卡里加「跟随动态元数据」开关（VSwitch，objectName `list-hdr-dovi-follow`），
   曲线没开时给出提示；文案进 i18n 四语。
3. 持久化走**已验通道**：`qml-preferences.v1.json` 的 `doviFollow`（全局兜底，启动时经
   `commitHdrTuning()` 应用）+ `hdrTitles[].doviFollow` / `hdrProfiles[].doviFollow`（每片/每显示）。
4. 顺手修掉 P1 的一个真 bug：`MediaFileSource` 里 `out.dovi = doviFrameInfo;` 原本写在提取**之前**，
   每帧带的是上一帧的 L1（曲线慢一帧、首帧无值）。已移到提取之后。
5. i18n：新增 4 条本功能文案；另补齐 P2b 遗留的 8 条杜比视界文案（杜比视界设置 / 未检测到片源 /
   RPU 有 / 无 RPU / P5 三条状态）。`python -B scripts/i18n/extract.py --check` →
   `missing={zh-TW:0, en:0, ja:0} placeholder problems=0`。

### 走错又退回的一段（教训写在 handoff 里）
先把 `doviFollow` 放进 `ChainGlobalSettings` 并把预设库提到 v14、会话提到 v9，再手写 v9 会话当夹具。
实测：会话被拒（`[ERROR] [ui-session] 模式会话损坏、无法读取或版本不支持`），且 bridge 的 `revalidate()`
会把 tuning 块打回默认。已全部回退（`git checkout` 三个文件），改用偏好通道。

### 关键发现
`EnhanceGraph::initialize()` 里 `desc_=desc` 会整体覆盖描述 → 任何新 live 字段都必须写进
`describeStages()`（`include/veyra/engine/GraphDescription.h`），否则图形重建时丢值。
这正是第一版只在 `applySettings` 里赋值时的现象：同一份数据目录两次运行，一次 follow=1 一次 follow=0。

### 实测证据（p8_30s.mkv / p5_30s.mkv，本机 RTX 5080，HDR10 输出）
| 用例 | 开关 | follow | applied | highlightStartNits |
|---|---|---|---|---|
| 空数据目录（曲线关） | off | 0 | 0 | 0 |
| 曲线开、跟随关 | off | 0 | 1 | 2000（= 用户值，画面不变） |
| 曲线开、跟随开（偏好驱动） | on | 1 | 1 | 286（0.85×336 nit） |
| 同一段第 200 帧 | on | 1 | 1 | 141（0.85×165.9 nit）/ cap 166 |
| P5 片源、跟随开 | on | 1 | 0 | 277（0.85×326.2 nit），P5 转换路径不受影响 |

全部 `--exit-after` 退出码 0；日志无 `Unexpected token` / `no root object` / `Type … unavailable`。
单元/回归：hdr_tuning 0 失败、preset_library 0 失败、ui_i18n 0 失败、repair_contract 280/0、
effect_chain 全过、vfg_settings 683/0（后两类需先移走 %TEMP% 里的测试残留目录）。
`veyra_settings_transaction_tests` 本机 8 轮 HDR 比对 `passed=0`；用 `git stash` 在无 DV 改动的基线上复测
同样 passed=0 → 与本功能无关。

### 未做
- P3-4 与 mpv（gpu-next）同片对拍观感
- P4 HDR10 元数据一致性（跟随 DV 时显示端元数据是否按片段上报）
- 未推送任何提交

### 2026-10-17 补记：P3-3 修正（上限不是盖子）与 P3-4 对拍
- **实测反馈"跟着开画面都有变化"是正确指控**：第一版跟随把 `peakCapNits` 也写成帧峰值，
  而它就是中灰 gamma 的锚点（g=log(mid/peak)/log(203/peak)）。用用户真实档位（mid 175 /
  start 450 / cap 1200）算：1 nit 掉 78%、100 nit 掉 18%、1000 nit 被硬夹掉 61%，
  逐帧还会呼吸。修正为只改 `highlightStartNits` 且单向下调（不削弱用户已设的压缩），
  3 条契约测试锁死（上限原样、1~203 nit 完全一致、低起点保留）。提交 4f24d9bd。
- 滚降区间（dims[12]）在跟随时按内容自身峰值收口，`dims[13]` 硬上限仍是用户值，
  否则高显示峰值会把坡度拉平到等于没压。
- P3-4 与 mpv v0.41 / libplacebo v7.376 对拍（p8_30s.mkv）：mpv 识别 DV profile 8，
  HDR10 PQ 直出同款 swapchain；libplacebo 的 tone_map 是线性 `a*x+b`，无 BT.2390/
  样条/Reinhard 痕迹 → 面板能力足够时**专业实现也不压**。我们同一帧 336 nit 处 -0.62%，
  1~320 nit 0.00% → 策略一致，不需要调 0.85 系数。
- 顺带确认产品截图不含呈现曲线（`EngineController` 保存的是 `videoFrameResource` 工作帧，
  HDR 时存 .jxr），所以"跟随/曲线"的效果不能靠截图对拍，只能靠曲线数学 + 运行日志。

### ✅ 2026-10-16 界面卡片化：复查、提 PR、以及两个新坑

### 做了什么
把 HDR 亮度曲线面板从平铺长列表重排为五张可折叠卡片（曲线图、读数与提示 / 一键选档 / 基础设置 / 高级设置：技术参数 / 记忆与自动化），
卡片直接复用作者自己的 `VAccordion` + `enabledSwitch: false`（因此边框、头部、折角、展开动画与色彩页一致），
行内提示统一 `VRow.hintBelow`，数值列收窄、长说明移到卡片底部小字。

### 提交方式（重要：fork 场景不能做"链式 PR"）
* 试着新建 PR、base 指向 PR #44 的分支时，GitHub 直接拒绝：
  `No commits between Likely7:hdr-curve-metadata and georgeg911:hdr-panel-cardify` / `Base ref must be a branch`。
* 原因：**PR 的 base 必须是上游仓库里的分支**，而 `hdr-curve-metadata` 只在我们的 fork 里。
  对 `main` 新建又会把 #44 重复一遍，所以正确做法是**把这批提交推到 #44 同一分支**（fast-forward），
  并在 #44 下留说明评论：https://github.com/Likely7/Veyra-NRVideo/pull/44#issuecomment-6197464565
* 撤销方式：`git push -f fork ced1d8c2:hdr-curve-metadata`。

### 提 PR 前必须剔除的两类"假差异"
1. **行尾噪声**：`include/veyra/ui/QmlPlayerBridge.h` 显示 1375 行差异，忽略行尾后**真实差异只有 1 行**
   （`git diff --ignore-cr-at-eol` 一眼看穿）。提 PR 时要 `git checkout <base-branch> -- <file>` 把它还原，
   否则 PR 里会出现整文件重写。**规矩：凡是 diff 行数与文件行数同量级，先怀疑行尾。**
2. **本仓库自有文件**：`CUSTOM_LEDGER.md` 这类个人台账不进对外 PR；`i18n/catalog.json` 则必须带上（纯新增，保证 i18n 测试绿）。

### 新坑：重复跑测试会被"上一次的残留"污染
* 现象：`veyra_hdr_tuning_tests` 212/1、`veyra_preset_library_tests` 504/1，稳定复现，
  失败行都是 `check(library.load() && library.put(entry), ...)` —— `load()` 成功但 `put()` 被拒。
* 真因：这两个用例**复用固定名字的临时目录**（`%TEMP%\pr19-hdr-persistence`、`%TEMP%\veyra-preset-library-tests`），
  上一次运行写下的文件让这一次的 `put()` 失败。`PresetLibrary.*` 与 `PresetLibraryTests.cpp` 相对 v2.0.8 **零差异**，
  `ninja: no work to do` 也说明二进制与源码一致 —— 所以**不是代码问题**。
* 处理：删掉 `%TEMP%` 下这两个测试目录再跑 → 立刻 **213/0 与 505/0**。
  **规矩：持久化类测试失败时，先清它自己的临时目录再判断，别急着改代码。**

### 本次验证结果（提 PR 的证据）
`qmllint` 0 error；应用启动退出码 0；`ui_i18n` 0 failures；`hdr_tuning` 213/0；`preset_library` 505/0；
逐项核对五张卡片的 glyph/hue/open/summary 与 `hintBelow`（12 处）；确认没有任何测试/工具/C++ 依赖我们改过的标签与 objectName（0 处）。

### ✅ 2026-10-15 HDR 亮度曲线 + HDR 元数据：与作者对齐、修掉我自己的三个坑、PR #44

**已发起 PR**：https://github.com/Likely7/Veyra-NRVideo/pull/44
（分支 `hdr-curve-metadata`，**从上游 tag `v2.0.8` 直接开出**，10 文件 / +1664 −18；
后追加 `ced1d8c2` —— 点亮跟随最后点击那个系统）

### 从 `custom` 剥出干净 PR 分支的做法（可复用）

1. `git checkout -b <pr> v2.0.8` —— **绝不基于 custom**（那样会把 19 个无关定制一起带上）
2. 逐文件：`git checkout v2.0.8 -- <file>` 回到上游原文 → `git diff v2.0.8 custom -- <file>` 取补丁
   → 按 hunk 判断（含 HDR 关键字 → 保留）→ `git apply --recount`（**位置由 git 保证**，不靠手写匹配）
3. 每次都做**三重复核**：文件**行数**是否合理、有无**重复行**、**夹带关键字是否为 0**
4. 最后在分支上**独立编译 + 跑全套测试** —— 只有它能证明补丁自洽完整
   （本次：编译 exit 0、`hdr_tuning` 213 checks、GPU/effect_chain/preset_library/ui_i18n/vfg_settings/lmxxf 全绿）

### 三个坑（我犯的，务必别再犯）

1. **块级回退把文件改坏** ✗：用 difflib 按"块"回退非 HDR 改动时对齐错位，`EngineController.cpp`
   出现**重复行**（编译不过）。**教训**：整块替换不安全 → 用 **hunk 级 `git apply`**。
2. **补丁拼接留空行** ✗：`split("\n")` 的末尾空元素混进 hunk → `git apply` 直接失败。
   **教训**：用 `splitlines()`。
3. **误丢 hunk** ✗：保留清单漏了「曲线生效时会按曲线后的输出上报」这句 → 元数据面板提示语被丢掉。
   **教训**：保留清单**宁宽勿窄**，并把**丢弃的块逐块打印**出来人工过一眼。
   另有一处**误判**：把 `resizeSourceColor` 当成 DoVi 内容回退掉了 —— 它其实是我们的 HDR 源数值采集。

### 一致性清理（PR 前）

* **注释译成英文**（代码库约定）；界面 `qsTr` 字符串**保持中文**（上游的源串也是中文）
* **objectName 无需改名** ✓：上游自己就用 `list-hdr-curve` / `list-hdrcurve-` / `list-hdrmeta-`，
  我们的 `list-hdrcurve-*` **正是作者的前缀**（我一度想改成 `list-hdr-curve-*`，查证后放弃 ✗）
* **推送前置**：fork 里缺 `.github/workflows` → **必须**先 `gh auth refresh -s workflow`，
  否则 `git push` 会被 GitHub 拒绝（这次用户已授权 ✓）

### HDR 元数据（恢复上游实现 + 三处对齐）

* 当年为修"桌面卡顿（悬停掉帧）"把 `applyHdrMetadata` 整段移除；**2.0.8 实测不卡**（同一台 60Hz 投影仪 ✓）
  → 用**上游原文**恢复（36 行），并保留我们的 ③①：
  * **③** 片源非 HDR 且未手动填值 → 不发（此前会虚报兜底 **1000 nit**）
  * **①** `clampMetadataToCurve()`：上报值按**曲线真正执行的上限**收敛（复用已测的 `hdrEffectivePeakCapNits`）
  * **④** 面板加一句说明
* 「HDR 元数据」折叠面板**加回界面**（此前连 UI 一起去掉 → 引擎恢复了却无法启用 ✗）

### 预设点亮：从"按数值匹配"到"跟随最后点击"

实测：两套预设写**同一条曲线**（「不补偿」时 自由组合 == 整档预设），所以一条曲线会**同时**
被两套认定 → 切视图时另一套也亮，看起来像"两个模式同时生效"。

* 第一次尝试：给点亮条件加"当前模式"前缀 → 但**点档位不切模式** ✗ → 反而变成
  "点自由组合的档，整档预设那一行亮" ✗
* 第二次：点任意档位**同时切换视图** ✓
* 第三次（最终 ✓）：新增偏好 **`hdrAppliedFrom`（最后生效来源）**，**与 `hdrPresetMode` 分开** ——
  只在"真正应用"时更新（点档位 ✓、点一键推荐 ✓），**切视图不动它** ✓
  → 只点亮**最后点过的那一套**，另一套整行不亮 ✓（用户已确认"就是我想要的效果"✓）
* UI 文案：**整档预设 / 自由组合**（原 七档旧预设 / 内容×环境）；小标题「**选档方式**」；
  两套各有一行状态（**当前生效：整档预设「X」** / **当前组合：内容 X · 环境 Y** + 等价说明一行）
* **自伤二连**（都修好了）：复制 `Q_PROPERTY` 时漏了 **NOTIFY 信号声明** → 编译 `C3861` →
  改为复用已有的 `preferencesChanged`；以及 **`WRITE` 子句被改漏**（指向 `setHdrPresetMode`）→ 已修正。
  **教训**：复制属性行必须**逐字段**核对（READ / WRITE / NOTIFY 三处）。

---

## 附、已归档的升级记录

以下内容原在本文件，属于 2.0.x 时代的升级流水，当前版本已经是 2.2.0，已归档（git 历史可查）：

- v2.0.5 / v2.0.6 / v2.0.7 / v2.0.8 / v2.0.9 五次升级的冲突清单、哈希、测试项数
- HDR 亮度曲线「第二梯队」（人话滑杆、内容×环境两轴）与「三项增强」——相关实现已被上游收编
- HDR 元数据的恢复与再次移除过程 ——功能已删除

**升级流程本身**仍在 HANDOFF.md（构建部署）和 MY_WORK.md 第 4.2 节，不会丢。
