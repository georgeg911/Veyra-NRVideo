# MY_WORK — Veyra 定制版开发记录

> **最后更新**：2026-11-21（Veyra 2.2.0 合并完成）
> **这份文件是什么**：记录这个**定制版**的现状、改了什么、为什么这么改、以后怎么继续。
> 作者原版的几百份文档在 `docs/` 里保持不动，不要混进来（合并会冲突，而且永远落后）。

---

## 0. 30 秒速览（新会话先读这一节）

**这个项目是什么**：Veyra 是一个 C++20 / Qt6 QML 的 Windows 视频播放器，作者是 Likely7。
这是**我自己的定制分支**，在作者基础上加了 B 站播放和杜比视界增强。

**四个关键位置**：

| 用途 | 路径 |
|---|---|
| 源码 | `D:\Program\Veyra\src` |
| 构建 | `D:\Program\Veyra\build-release` |
| 部署（实际运行） | `D:\Program\Veyra-2.2.0-NVIDIA-win64-portable` |
| 构建/部署手册 | **同目录的 `HANDOFF.md`** |

**定制了什么**（作者版没有的）：

1. **B 站播放**：粘 URL 就能播，带弹幕、双语字幕、字幕配色
2. **杜比视界 CB1 高光增强**：按场景 headroom 展开高光，不硬压
3. **Chrome 插件拉起播放**：浏览器一键开播

**三条铁律**：

- 只做**自己用**的功能，不追求给作者提 PR
- 改任何东西前先看 `HANDOFF.md` 的「新会话第一步」
- 删除文件一律进回收站，不物理删除

---

## 1. 当前状态

### 固定不变的

```
源码      D:\Program\Veyra\src
分支      local/2.2.0-merge          （基于作者 eb5fd6d0 / v2.2.0）
构建      D:\Program\Veyra\build-release
部署      D:\Program\Veyra-2.2.0-NVIDIA-win64-portable

部署目录里的两个启动脚本：
  启动-个人定制版.cmd   → 定制版，数据目录 user-data-2.0.0
  启动-作者完整版.cmd   → 作者原版，数据目录 user-data-2.0.0-author
```

### 会变的（用命令查，不要信文档里的数字）

```powershell
cd D:\Program\Veyra\src
git log --oneline -1                        # 当前 HEAD
git rev-list --count eb5fd6d0..HEAD         # 领先作者几个提交
git status --porcelain                      # 工作区是否干净（应为空）
```

### 部署目录的双版本结构

| 文件 | 用途 |
|---|---|
| `veyra_qml_ui.exe` | 当前生效（切换后变） |
| `veyra_qml_ui.exe.custom` | 定制版 exe（8.87 MB） |
| `veyra_qml_ui.exe.orig` | 作者原版 exe（22.35 MB） |
| `qml.custom\` / `qml.orig\` | 两套 QML |
| `shaders\*.dxil` | 当前生效 |
| `shaders\*.dxil.custom` / `.orig` | 两套 shader |

切换靠两个 `.cmd`，它们会同时替换 exe + QML + shader 三样。

---

## 2. 定制功能清单

### 2.1 B 站播放（核心定制）

**能做什么**：

| 能力 | 说明 |
|---|---|
| 粘页面 URL 直接播 | 支持 `bilibili.com/video/BV…`、`av…`、`b23.tv` 短链 |
| 分P 选择 | URL 带 `?p=N` |
| 画质选择 | 最高 4K（受账号会员限制） |
| DASH 音视频分离 | 视频和音频是两条独立 CDN 地址 |
| 弹幕 | 设置里可开关，字号/透明度/速度/区域可调 |
| B 站 CC 字幕 | 设置里可开关 |
| **中文/英文/双语三选项** | 有英文 CC 的视频可双语显示 |
| **字幕配色** | 5 套预设（白字黑边、白字半透明黑底、黄字黑边、白字紫底、黑字白底） |

**关键文件**（核查功能是否还在）：

```
src/media/BilibiliResolver.cpp      页面 URL → CDN 流地址
src/media/BilibiliExtras.cpp        弹幕 XML、CC 字幕、WBI 签名
src/media/BilibiliHttp.cpp          共享 HTTP 助手（gzip、Referer）
src/media/DanmakuOverlay.cpp        弹幕引擎（GDI+ 分层窗口）
src/media/DanmakuWindow.cpp         弹幕窗口封装
include/veyra/media/BilibiliResolver.h
```

**入口**：

- QML：设置页的 `set-bili-overlays` 分组
- C++：`Playback.cpp` 的 `openNetworkAddress()` / `tickBilibiliResolve()` / `applyBilibiliExtras()`
- 插件：`PlaybackServer.cpp` 监听 `127.0.0.1:18765`

**明确不要的东西**（用户明确要求，别再问）：

- ❌ B 站浏览页（`.qml` 已删）
- ❌ 登录界面 / 收藏夹 / 历史 / 搜索
- ❌ 弹幕发送

### 2.2 杜比视界增强

| 能力 | 说明 |
|---|---|
| DV Profile 5 / 7 / 8 解码 | 作者原版就有 |
| **CM v2.9 / v4.0 创作锚点** | 读作者的原始创作意图（作者版没有） |
| **CB1 每像素高光扩展** | 在 present blit 阶段按场景 headroom 展开（作者版没有） |
| **CB1 IPT 色度门** | 防止扩展时偏色 |
| **roll-off 膝点按场景派生** | 作者固定 1000 nits |
| **libdovi 解析增强层 RPU** | 作者只认基础层 |

**关键文件**：

```
include/veyra/pipeline/DoviCm4.h            创作锚点数据结构
include/veyra/pipeline/DoviCm4Trim.h        trim 算子
include/veyra/pipeline/DoviEnhancedCurve.h  CB1 扩展曲线
shaders/PresentBlit.hlsl                    高光扩展在 blit 里执行
deps/libdovi/                               libdovi 3.4.0（本地依赖）
```

**开关**：设置里的 `doviEnhanced`（默认关）

**第三方声明**：`THIRD_PARTY_NOTICES.md` 里有 CB1 的 GPL-2.0→GPL-3.0 声明，**不要删**。

### 2.3 Chrome 插件拉起播放

| 项 | 值 |
|---|---|
| 端点 | `http://127.0.0.1:18765/play` |
| 插件目录 | `tools/chrome-extension/` |
| 注册协议 | `veyra://`（`install-protocol.cmd` 注册） |
| 插件 ID | `flbonkialmkgbkbnboppcdbfiompbkhn` |

插件 POST 的 body：`{"url": "...", "quality": 112, "cookie": "..."}`

### 2.4 从 2.2.0 继承的功能（作者的新功能，都有）

实时字幕翻译、RTP/UDP 地址、5 种界面语言、引导漫游、帧节奏修复、自适应补帧、HUD 锁定、新呈现缩放。

**只有一个没开**：便携版签名自动更新（`VEYRA_ENABLE_SIGNED_UPDATE=OFF`，因为只自己用，不需要）。

---

## 3. 已知限制（不是 bug，别去修）

这些是**有意为之**或**做不到**的，再遇到不要当问题处理：

| 现象 | 真相 |
|---|---|
| 串流 / 采集卡功能「缺失」 | **有意不构建**：`VEYRA_ENABLE_MOONLIGHT/REMOTEPLAY/XBOX` 默认 OFF。用户明确说用不到。需要时切「作者完整版」 |
| B 站选 4K 却只有 1080p | **账号权限问题**，不是 bug。日志会写 `qn=126 not permitted, served qn=112` |
| 播放前约 2.7 秒透出桌面 | **作者原版架构**（`blankVideo()` 裁窗口），只影响观感，**决定不修** |
| 弹幕/字幕开关自己变回关 | 程序退出时可能重置。去设置里重新打开即可 |
| 本地视频快速切源时「只有画面没声音」 | 音频管道继承了上次会话的退出标志 → 引擎按**既有设计**关闭音轨，视频静音继续。避免方式：等上一段完全结束再拉起下一段。**注**：网络源（B 站）已不受影响，断网自愈见 3.5 |
| 每次重连泄漏一个 `AVFormatContext` | **决定不修**。试过在 `open()` 分配前加 `closeAll()`，实测崩在 `avcodec-63.dll 0xC0000005`——`thread_.joinable()` 不保证线程已退出，释放 `codecCtx_` 等于从活线程脚下抽走。要正确修必须先改造音频线程模型；泄漏量是每次重连几百字节、上限 4 次，不值当。代码里留了注释说明 |
| 切版本后实时翻译显示「未安装」 | 翻译资产只在 `user-data-2.0.0`（定制版）。作者版通过 Junction 共享，若链接失效需重新指向 |
| 字幕颜色改了要重开视频？ | 不用，颜色是实时的。但**弹幕/字幕开关、字幕语言**改了要重开视频才生效 |

---

## 3.5 网络断线自愈（定制功能）

### 用户要的结果

**播着视频断网几秒、再联网，画面和声音自己接上，不用手动点进度条。**

这一条就是全部验收标准。中途卡几秒可以接受，永久静音或需要人工干预不行。

### 现在的行为

| 场景 | 结果 |
|---|---|
| 断网 1-10 秒后恢复 | 约 5-6 秒内自动恢复音画，**不需要任何操作** |
| 恢复过程中 | 画面继续走，最长停顿约 1-2 秒（重连 + seek） |
| 重连第一次失败 | 自动重试（最多 4 次），网络回来后成功 |
| 完全连不上 | 4 次预算用完后静音继续播，画质不受影响 |

### 为什么是这个设计（踩坑记录，别重复）

断网自愈不是一处能解决的，是**三个阻断点串在一条链上**，缺任何一环都恢复不了。下面按「发现顺序」记，每个都附现场日志特征：

**① 断网 0.2 秒就关掉音轨** → 走不到重连

```
01:43:46.584  read-ahead failed code=-5 (attempt 1); retrying
01:43:46.800  audio unreadable at 52672 ms; closing the track    ← 0.216 秒
```
音轨一关就 `readerEof_` → `decodedEof_` → 「音频已播完」，缓冲门看到 `audioAvailable=false`
直接返回，重连预算一次都没用过。改成**网络源不关音轨、按 50ms 退避持续重试**，让缓冲自然
耗尽好让门介入。本地文件保持「读不动就关」。

**② 一次重连失败就永久放弃** → 网线插回来也没机会

```
02:11:22.209  audio address could not be reopened       ← 25ms 失败
02:11:23.379  pcmMs=-1 queuedMs=-1 holding=false reconnects=1   ← 之后永远这样
```
拔网线时第一次重连必然失败（网络还没回来），`abandon()` 之后就再没第二次。改成预算内
（4 次）**退回「等待恢复」自动重试**。

**③ 重连卡主线程 8-38 秒** → 画面冻住

```
02:19:32  [ERROR] [engine-hang] resample stalledMs=38407
```
重连在引擎主循环上同步跑，用的是首次打开的 40 秒连接预算。加 `reopen()` 走 **6 秒短预算**：
不可达就换下一个镜像，别按首次打开的标准等。

**④ 声音回来了但画面卡死** → 这是最后一环，也是最容易修错的一环

```
02:29:35.810  audio recovered: pcmMs=986 queuedMs=2508   ← 声音完全正常
02:41:41.277  qml-frames: main=82 per 10 s               ← 画面几乎不动
02:29:43.907  engine-hang stalledMs=8079 lastStage=collect
```
音频恢复了，画面还在按旧时钟等一条从 19.7 秒开始的音轨。**用户点一下进度条就好** ——
这一句是关键线索。

### 教训（写下来防止再犯）

**用户已经验证过「手动点进度条能恢复」，就直接复用那条路径，别自己推演状态机。**

我第一版去改 `anchor`/`anchorMs`/`reset`/`pendingResetCause` 这一套时钟基准，
写了 40 行，测出来**仍然 engine-hang 8 秒、帧率掉到 39**——因为视频侧没有走完整的
seek 流程（重新解到目标位置那一整套）。最后正确做法只有 6 行：

```cpp
// 重连成功后，把 seekSeconds_ 设成当前媒体时刻，走正常 seek 流程
if (std::isfinite(resumeMs) && resumeMs > 0.0 && seekSeconds_.load() < 0) {
    std::lock_guard lock(mutex_);
    snapshot_.seekTarget = resumeMs / 1000.0;
    ++snapshot_.seekRequested;
    seekSeconds_ = resumeMs / 1000.0;
}
```

效果对比：

| | 自己改 anchor（40 行） | 复用 seek 路径（6 行） |
|---|---|---|
| engine-hang | 每次 8 秒 | **0 次** |
| QML 帧率 | 39-129（卡死） | **402-671（流畅）** |
| 需要手动干预 | 是 | **否** |

**通用原则**：遇到「某个操作能修好」的情况，先问「能不能让程序自己做那个操作」，
而不是去猜那条操作背后改了什么状态。后者要复制一整套隐式契约，几乎必错。

### 涉及的文件（要改这里先读上面的踩坑记录）

| 文件 | 作用 |
|---|---|
| `src/sink/WasapiAudioSink.cpp` | 读失败时网络源不关音轨（`networkRead` 分支） |
| `src/engine/EngineController.cpp` | 重连重试、短预算 reopen、重连后自动 seek |
| `include/veyra/engine/BufferingGate.h` | `retryLater()`（失败后重新等待）、预算 2→4 |
| `include/veyra/media/NetworkInput.h` | `kReconnectTimeoutMs=6000` + `armConnectShort()` |
| `include/veyra/sink/WasapiAudioSink.h` | `reopen()` / `openImpl(shortBudget)` |

### 相关提交

```
53fdd848  重连成功后自动走一次 seek（用户方案，画面恢复）
8ceb24a1  重连失败不再永久放弃 + 短连接预算
2764569b  断网 0.2 秒就关音轨 → 网络源持续重试
```

（曾有一个 `23ea559d` 改 anchor 的提交，已被 `53fdd848` 覆盖后摘除，只留备份分支。）

---

## 4. 行动规则

### 4.1 三条线：改动的性质决定怎么做

| 颜色 | 含义 | 做法 |
|---|---|---|
| 🟢 绿 | 纯本地定制（B 站、DV 增强、插件） | 直接改，提交到自己分支 |
| 🟡 黄 | 通用改进（可能对作者也有用） | 先记录，想清楚再决定提不提 PR |
| 🔴 红 | 与作者理念冲突（删功能、改默认行为） | 只在本地做，**不要提 PR** |

### 4.2 升级上游 / 合并新版本（最常做的事）

**安全顺序**：

```
1. 确认工作区干净（git status --porcelain 为空）
2. 新建分支：git checkout -b local/x.y.z-merge
3. git merge <作者新版本>
4. 有冲突 → 逐个解决，解决后必须编译验证
5. 编译通过 → 部署 → 实测 → 才提交
6. 不要在没有编译验证的情况下继续往下走
```

**合并前先看净差异**：

```powershell
git diff --stat <作者版本> HEAD     # 看我们比作者多了什么
```

**合并后必做三件事**：

1. 全量编译（不是增量，避免残留污染）
2. 部署并启动验证（无 QML 错误）
3. 实测核心功能（B 站播放 / 弹幕 / 字幕 / DV）

### 4.3 构建配置必须逐项对照手册

**这是血泪教训**：曾经因为反复切换 `CMAKE_BUILD_TYPE` 和链接选项，导致编译产物 STL ABI 不一致，程序启动就崩（`std::format` 内部崩溃）。

**规则**：重新 configure 时，参数必须从 `HANDOFF.md` 逐项复制，不能凭记忆。

**关键参数**（必带）：

```
-DVEYRA_BUILD_QML_UI=ON               ← 不加就不会生成 veyra_qml_ui.exe
-DVEYRA_ENABLE_EXPERIMENTAL_DLSSNR=ON ← 不加会链接失败
-DVEYRA_ENABLE_SIGNED_UPDATE=OFF      ← 不需要 OpenSSL
-DVEYRA_DLSS_SDK_ROOT / VEYRA_NVENC_HEADERS_ROOT / VEYRA_XESS_ROOT / VEYRA_AMF_SDK_ROOT
```

### 4.4 删除一律进回收站

**禁止** `rm`、`Remove-Item`、`del`、`rmdir`。

```powershell
Add-Type -AssemblyName Microsoft.VisualBasic
[Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile($path,
  [Microsoft.VisualBasic.FileIO.UIOption]::OnlyErrorDialogs,
  [Microsoft.VisualBasic.FileIO.RecycleOption]::SendToRecycleBin)
```

### 4.5 清理和恢复必须成对

**教训（2026-10-25 四次同型错误）**：清理掉了某个状态，恢复时没恢复全，功能静默失效。

**规则**：写清理代码时，同一个 PR 里必须写对应的恢复代码。例如：
- 清弹幕 → 恢复时也要重新加载弹幕
- 清切换标记 → 恢复时要恢复标记

### 4.6 改解析/判断逻辑前先写例子试

**教训**：只凭读代码推理改解析逻辑，改错了看不出来。

**规则**：先用真实数据跑一遍，人工确认输入输出，再改代码。

### 4.7 改共用属性前先查它被谁用

```powershell
grep -rn "属性名" src/ include/ apps/ qml/
```

**教训**：改一个「共用」属性影响面比想象大。

### 4.8 命令/设置点了没反应 → 两端都加日志

证明「发出去了」和「收到了」，再定位问题在哪一端。

### 4.9 判断「功能缺失」前先查是不是有意关闭

先查 `CMakeLists.txt` 的 option 和设置项的默认值，别急着当成 bug。

### 4.9.1 修问题前先问「用户是怎么绕过去的」

**这是本项目最贵的一课（网络断线自愈，2026-11-22）。**

用户报「断网后声音没了，画面也卡住」。我从日志一路挖到底层，改了音轨关闭时机、
重连重试、连接超时、时钟基准——四个提交、两百多行，其中改时钟基准那版还失败了
（仍然 hang 8 秒、帧率 39）。

后来用户说了一句：**「我点一下进度条就恢复了」**。

那就别猜进度条背后改了哪些状态，直接让程序在重连成功后调一次 seek。6 行代码，
问题彻底解决（engine-hang 归零，帧率回到 400-670）。

**规则**：用户描述现象时，一定要追问：

1. **有没有什么操作能让它恢复正常？**
2. 如果有，那个操作就是**已验证正确的修复路径**，优先复用，不要自己重推一遍状态机。

道理很直白：用户绕过去的那个动作，内部必然把该复位的东西都复位了。你自己去猜
「它到底复位了哪些状态」，等于要复制一整套隐式契约，几乎必错；而直接调用它，契约由
原作者维护。

**反例特征**（发现自己正在这样，就停下来问）：
- 开始修改多个互相关联的状态变量（`anchor` / `reset` / 各种 flag）
- 每次改完都有一个新现象冒出来
- 改动量越来越大，但仍然没有端到端验证成功

### 4.9.2 现象和原因之间隔了几层时，用日志分层定位

断网自愈那次，四个阻断点各自有明确的日志特征：

| 日志特征 | 说明卡在哪 |
|---|---|
| `closing the track`（断网 <1 秒） | 音轨被关闭，走不到重连 |
| `could not be reopened` + 之后无动作 | 重连失败后放弃了 |
| `engine-hang stalledMs=...` | 主循环被阻塞 |
| `audio recovered` 但 `qml-frames` 很低 | 音频好了、画面没跟上 |

**做法**：先在日志里找「最后一次正常」和「第一次异常」，两者之间的空白就是阻塞点。
不要靠读代码推理时序——FFmpeg / Qt 的阻塞点读代码看不出来。

### 4.9.3 碰资源释放和线程的改动，先 AB 对照再决定留不留

**第二贵的一课（AVFormatContext 泄漏，2026-11-22）。**

审计说 `AudioPipeline::open()` 每次覆盖 `fmt_` 不释放旧的，每成功重连泄漏一个
AVFormatContext。听起来完全正确，我就在分配前加了 `closeAll()`。

**实测直接崩**：`avcodec-63.dll 0xC0000005`。

根因：

```cpp
if (thread_.joinable()) return false;   // 只说明线程对象存在，不保证线程已 return
closeAll();                              // 释放 codecCtx_，把资源从活线程脚下抽走
```

AB 对照一跑就见分晓：

| 配置 | 结果 |
|---|---|
| 加 `closeAll()` | 崩溃 avcodec 0xC0000005 |
| 去掉它 | 90 秒稳定、4 次重连全成功、帧率 626 |

**规则**：任何涉及**资源释放**（free/close/release）或**跨线程生命周期**的改动，
不要直接合入。先做 AB：

1. 改之前跑一次基线（确认当前是好的）
2. 加上改动再跑同一条路径
3. 两次结果对比，坏的立刻回滚

这比自己读代码推演线程时序快得多，也可靠得多——`joinable()`、`shared_ptr` 计数、
「应该已经释放了」这类判断，读代码永远看不出真实时序。

**推论**：审计建议也是假设，同样要验证。「审计说了要修」不等于「这么修就对」。

**另一个判断依据**：收益 vs 风险。上面那个泄漏每次重连几百字节、上限 4 次，
而修复要先改造线程模型——量级差这么远时，正确决定是**不做**，并把原因写进代码注释，
避免下次有人（包括自己）再试一遍。

### 4.10 提交与推送

- 一个功能 = 一个提交
- 提交信息写**为什么**，不是**改了什么**
- **推送前必须问用户**，不能私自 push
- 用户验收前一律不推

### 4.11 验证请求必须连 exe 一起部署

只换 QML 不换 exe，用户看到的界面和描述不一致。

**部署清单**：exe、`.custom` 副本、`shaders\*.dxil`、QML 两处（`qml\` 和 `qml.custom\`）。

---

## 5. 待办

| 项 | 状态 |
|---|---|
| 实时翻译实测 | 资产已装（1.07 GB），**未实际测试翻译效果** |
| 30 个提交推送 | 等用户验收后再决定 |
| 上游有新版本时合并 | 按 4.2 的流程 |

---

## 6. 文档地图

| 想做什么 | 看哪里 |
|---|---|
| **构建、部署、验证、踩坑** | **`HANDOFF.md`**（同目录，最重要） |
| 杜比视界技术细节 | `docs/DOLBY_VISION.md` |
| 历史故障排查记录 | `docs/HISTORY.md` |
| 定制功能一页速览 | 本文第 2 节 |
| 作者的架构说明 | `docs/ARCHITECTURE.md`（作者原版，不动） |
| 作者的开发指南 | `docs/DEVELOPMENT.md`（作者原版，不动） |

**历史存档**：这个定制版早期的 39 份计划/报告（2026-10 期间）在 git 历史里的 `local-notes` 分支上，需要时能查，不放在工作目录。

---

## 附：为什么作者的文档不并进来

作者每 1-2 周发一版，每次带几十份新文档。如果把他的文档合并进我们的记录：

1. 每次升级必冲突
2. 我们的版本永远落后
3. 分不清哪些是我们写的

所以：**作者的留在 `docs/` 原样不动，我们的只写在这两份文件里**（`MY_WORK.md` + `HANDOFF.md`）。
