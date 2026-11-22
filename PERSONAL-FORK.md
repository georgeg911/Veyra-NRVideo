# Personal customization branch / 个人定制分支

**English** · [中文](#中文说明)

---

## English

This branch is a **personal customization** of [Veyra](https://github.com/Likely7/Veyra-NRVideo)
built on top of the official **2.2.0** release. It is not affiliated with the author and is
not a pull request candidate — several features here are things the upstream project
deliberately does not do (Bilibili page playback in particular).

It exists mainly so the work survives a disk failure. Nothing here is meant to be merged
upstream.

### What was added

**Bilibili playback**

- Play a Bilibili page URL directly, from both the player and the browser extension.
- Multi-part videos and collections: every episode is queued into the playlist with the
  site's own titles, and the row that is playing is highlighted.
- Chinese / English / bilingual CC subtitles, switchable in Settings.
- Danmaku, with its own toggle.
- Audio mirror rotation: when a CDN edge drops, the next one listed by the playurl
  response is used instead of reopening the same dead address.

**Network resilience**

- **Audio self-healing after a network drop.** Upstream closes the audio track about 0.2 s
  into an outage, which means the reconnect path never runs and the rest of the video plays
  silently until the user seeks by hand. Fixed: the track stays open and reconnects within
  roughly 5–6 seconds of the link coming back.
- Reconnects no longer give up after the first failure, and use a short connect budget so
  they cannot freeze the main loop.

**Local file tolerance**

- Damaged or partially downloaded files play instead of failing with "corrupted data".
  Bad packets are skipped and decoding continues; the reliable recovery is a manual seek
  (right arrow).

**Playback and UI**

- Subtitle size in seven steps (75% – 200%).
- Dolby Vision highlight expansion (CB1 per-pixel expansion, CM4 trim weighting).
- Fixes from a self-run audit: dead code, duplicated helpers, key normalization, and a
  Qt 6 QML crash in multi-part playback.
- `/play` request body limit raised to 256 KB for large part lists.

### Not upstream material

- Bilibili support: the author does not do page playback, so this stays local.
- Everything personal: deployment scripts, build configuration for this machine, and the
  working notes are deliberately **not committed** to this branch.

---

## 中文说明

本分支是 [Veyra](https://github.com/Likely7/Veyra-NRVideo) 官方 **2.2.0** 版本之上的
**个人定制**。与作者无关，也不是 PR 候选——其中若干功能（尤其是 B 站页面播放）是上游
明确不做的。

这个分支存在的主要目的是**防止硬盘故障导致工作丢失**，没有合并回上游的打算。

### 增加了什么

**B 站播放**

- 直接粘贴 B 站页面网址播放，播放器内和浏览器扩展两个入口都支持。
- 多P 视频与合集：全部分集带站内真实标题加入播放列表，正在播的那一条高亮显示。
- 中文 / 英文 / 双语 CC 字幕，设置里可切换。
- 弹幕，带独立开关。
- 音频镜像轮换：某个 CDN 边缘挂掉时，改用 playurl 返回的下一个镜像，而不是反复重开
  那个已经失效的地址。

**网络自愈**

- **断网后音频自动恢复。** 上游在断网约 0.2 秒时就关闭音轨，导致重连逻辑根本没机会
  执行，之后整段视频静音，只能手动拖进度条。已修：音轨保持打开，网络恢复后约 5-6 秒
  内自动接上。
- 重连不再第一次失败就放弃，并改用短连接预算，避免卡住主循环。

**本地文件容错**

- 损坏或没下载完的文件能播放，不再直接报「源视频包含损坏数据」。
  坏包跳过、解码继续；可靠的恢复手段是手动 seek（右箭头）。

**播放与界面**

- 字幕大小 7 档（75% – 200%）。
- 杜比视界高光扩展（CB1 逐像素扩展、CM4 trim 加权）。
- 自己跑的审计修复：死代码、重复实现、键归一化，以及多P 播放的 Qt6Qml 崩溃。
- `/play` 请求体上限提到 256 KB，支持更大的分集列表。

### 不属于上游范围

- B 站支持：作者不做页面播放，所以只留在本地。
- 所有个人内容：部署脚本、这台机器的构建配置、工作笔记都**刻意没有提交**到这个分支。
