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
| 「只有画面没声音」（快速切源时） | 音频管道继承了上次会话的退出标志 → 引擎按**既有设计**关闭音轨，视频静音继续。避免方式：等上一段完全结束再拉起下一段 |
| 弹幕/字幕开关自己变回关 | 程序退出时可能重置。去设置里重新打开即可 |
| 切版本后实时翻译显示「未安装」 | 翻译资产只在 `user-data-2.0.0`（定制版）。作者版通过 Junction 共享，若链接失效需重新指向 |
| 字幕颜色改了要重开视频？ | 不用，颜色是实时的。但**弹幕/字幕开关、字幕语言**改了要重开视频才生效 |

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
