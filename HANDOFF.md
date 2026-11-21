# HANDOFF — 构建、部署、验证手册

> **最后更新**：2026-11-21（Veyra 2.2.0）
> **这份文件是什么**：环境操作手册。新会话照着做就能构建、部署、验证。
> 项目状态和开发规则在 `MY_WORK.md`。

---

## 一、铁律：构建路径必须是纯 ASCII

CMake 的 Ninja 生成器写不出含中文路径的 `rules.ninja`，构建会报 `unknown build rule`。

**所以**：源码、构建目录、临时脚本，**全部放在纯 ASCII 路径**。当前路径已经是好的，不要改。

---

## 二、目录结构

```
D:\Program\Veyra\
├── src\                    源码仓库
├── build-release\          构建目录（Release）
├── deps\ffmpeg\            FFmpeg 运行时依赖
└── ...

D:\Program\Veyra-2.2.0-NVIDIA-win64-portable\     ← 部署目录（实际运行）
├── veyra_qml_ui.exe              当前生效
├── veyra_qml_ui.exe.custom       定制版（8.87 MB）
├── veyra_qml_ui.exe.orig         作者原版（22.35 MB）
├── qml.custom\  qml.orig\        两套 QML
├── qml\Veyra\                    当前生效 QML
├── shaders\*.dxil                当前生效 shader
├── shaders\*.dxil.custom/.orig   两套 shader
├── 启动-个人定制版.cmd
├── 启动-作者完整版.cmd
├── 打包分享版.cmd                 生成分享包（排除个人数据）
├── install-protocol.cmd          注册 veyra:// 协议
└── runtime_local\
    ├── amd\  intel\              FSR / XeSS 运行时（必需）
    ├── user-data-2.0.0\          定制版数据（含翻译资产 1.07 GB）
    └── user-data-2.0.0-author\   作者版数据（live-translation 是 Junction）
```

---

## 三、工具链确切位置

| 工具 | 路径 |
|---|---|
| CMake | `C:\Program Files\CMake\bin\cmake.exe` |
| Ninja | `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe` |
| MSVC | `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat` |
| Qt 6.8.3 | `D:\Program Files\Qt\6.8.3\msvc2022_64` |
| FFmpeg | `D:\Program\Veyra\deps\ffmpeg\ffmpeg-n9.0.1-11-ge47273f4d9-win64-lgpl-shared-9.0` |

---

## 四、构建

### 4.1 全量配置（只在首次或改 CMakeLists 后需要）

写一个**纯 ASCII 的 .cmd**，不要用工具写含中文的 bat（cmd 按 GBK 读会乱码）：

```bat
@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
"C:\Program Files\CMake\bin\cmake.exe" -S D:\Program\Veyra\src -B D:\Program\Veyra\build-release -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH="D:/Program Files/Qt/6.8.3/msvc2022_64" ^
  -DVEYRA_FFMPEG_ROOT=D:/Program/Veyra/deps/ffmpeg/ffmpeg-n9.0.1-11-ge47273f4d9-win64-lgpl-shared-9.0 ^
  -DVEYRA_LIBDOVI_ROOT=D:/Program/Veyra/src/deps/libdovi ^
  -DVEYRA_ENABLE_LIBDOVI=ON ^
  -DVEYRA_BUILD_QML_UI=ON ^
  -DVEYRA_ENABLE_SIGNED_UPDATE=OFF ^
  -DVEYRA_ENABLE_EXPERIMENTAL_DLSSNR=ON ^
  -DVEYRA_ENABLE_D3D12_DEBUG=ON ^
  -DVEYRA_ENABLE_DEINTERLACE=ON ^
  -DVEYRA_DLSS_SDK_ROOT=D:/Program/Veyra/src/third_party_local/nvidia/DLSS-SDK-310.7 ^
  -DVEYRA_NVENC_HEADERS_ROOT=D:/Program/Veyra/src/third_party_local/nvidia/nv-codec-headers-13.0 ^
  -DVEYRA_XESS_ROOT=D:/Program/Veyra/src/third_party_local/intel/xess-3.0.2 ^
  -DVEYRA_AMF_SDK_ROOT=D:/Program/Veyra/src/third_party_local/amd/AMF-SDK-1.5.3 ^
  -DVEYRA_FIDELITYFX_ROOT=D:/Program/Veyra/src/third_party_local/amd/FidelityFX-SDK/sdk ^
  -DVEYRA_FIDELITYFX2_ROOT=D:/Program/Veyra/src/third_party_local/amd/FidelityFX-SDK-2.3.0/Kits/FidelityFX ^
  -DVEYRA_CUDA_DRIVER_INCLUDE_DIR=D:/Program/Veyra/src/third_party_local/nvidia/Optical_Flow_SDK_5.0.7/NvOFFRUC/NvOFFRUCSample/cudart_header ^
  -DVEYRA_DISPLAY_VERSION=2.2.0
```

### 4.2 三个必须给的开关（少一个就出问题）

| 参数 | 漏了的后果 |
|---|---|
| `VEYRA_BUILD_QML_UI=ON` | **不生成 `veyra_qml_ui.exe`**（默认 OFF，构建成功但没主程序）|
| `VEYRA_ENABLE_EXPERIMENTAL_DLSSNR=ON` | `veyra_ngx` 目标不生成，**链接直接失败** |
| `VEYRA_CUDA_DRIVER_INCLUDE_DIR=…` | **VFG 静默禁用**（构建成功，但补帧选项里 NVIDIA VFG 永远灰的，不报错）|

第三个尤其阴险：不报错、不警告。验收方法：配置后检查 `build-release/build.ninja` 里有 `VEYRA_HAVE_CUDA_DRIVER_HEADERS=1`。

### 4.3 增量构建（日常）

用项目自带的脚本（满核并行）：

```bat
D:\Program\Veyra\src\scripts\build-local.cmd
```

它做的事就是：

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
"C:\Program Files\CMake\bin\cmake.exe" --build D:\Program\Veyra\build-release -j 16
```

**`-j 16` 是满核**（本机 16 逻辑核）。以前用 `-j 8` 只用了一半，全量慢一倍。

**真实耗时**（本机实测，RTX 5080 / 16 核）：

| 场景 | 编译步数 | 耗时 |
|---|---|---|
| 无改动 | 5 | **3 秒** |
| 改 20 个 `.cpp` | 142 | **16 秒** |
| 改 1 个被广泛引用的头文件 | 224 | **66 秒** |
| 全量重建 | ~700 | 约 2-3 分钟 |

### 4.3.1 绝对不要把构建输出接管道

**这是踩过的坑**：用 `build.cmd | grep ... | head` 时，`head` 拿够行数就退出，但上游 `cmd.exe` 还在写 stdout，管道缓冲区满后阻塞，**批处理看起来永远跑不完**。实测编译早在 14:28:58 就完成了，命令却卡到 14:33 还在"等待"。

正确做法：

```powershell
# 输出重定向到文件，前台等待
cmd /c D:\Program\Veyra\src\scripts\build-local.cmd > D:\DSH\.dsh-tmp\build.log 2>&1
# 编译完再读文件
Get-Content D:\DSH\.dsh-tmp\build.log -Tail 20
```

**判断编译是否结束，看 exe 时间戳，不要看命令状态**：

```powershell
(Get-Item D:\Program\Veyra\build-release\veyra_qml_ui.exe).LastWriteTime
```

### 4.3.2 改头文件会触发大面积重编

改了被广泛 include 的头文件（`FramePacket.h`、`VideoPresenter.h`、`QmlPlayerBridge_p.h` 等），会连锁重编几十上百个 `.cpp`，这是 C++ 的固有代价，无法避免。

**所以：把同一批改动攒完再编译一次，不要边改边编。**

| 改动类型 | 触发范围 | 建议 |
|---|---|---|
| 只改 `.cpp` | 1-20 个文件 | 随时编（10-20 秒） |
| 改 HLSL | 重新 dxc 编所有 shader | 攒着一起改 |
| 改头文件 | 几十到几百个文件 | **攒完所有改动再一次编** |

### 4.4 什么时候必须全量重建

**切分支、合并上游、改 CMAKE_BUILD_TYPE / 链接选项之后，必须清干净再编。**

否则 .obj/.lib 混用，会出现：
- `LNK4098`（CRT 冲突）
- `LNK2019`（找不到符号）
- **更糟：编译成功但运行时崩**（STL ABI 不一致，`std::format` 内部崩溃）

清理方式：删掉 `build-release` 目录重新配置（最干净），或 `ninja -t clean`。

**教训（2026-11-20）**：反复切换 `CMAKE_BUILD_TYPE` 和 `/DEBUG` 链接选项，导致部分 obj 用一种 STL 配置、部分用另一种，程序启动就崩（`0xC0000005`）。统一配置后立即正常。

### 4.5 有意关闭的功能（不是遗漏，不要"修"）

| 功能 | 关闭方式 | 原因 |
|---|---|---|
| Moonlight / Sunshine 串流 | `VEYRA_ENABLE_MOONLIGHT=OFF`（默认）| 用户用不到 |
| PS5 Remote Play | `VEYRA_ENABLE_REMOTEPLAY=OFF`（默认）| 用户用不到 |
| Xbox 串流 | `VEYRA_ENABLE_XBOX=OFF`（默认）| 用户用不到 |
| DeckLink / Magewell 采集 | SDK 路径留空 | 用户用不到，且需要外部 SDK |
| 便携版签名自动更新 | `VEYRA_ENABLE_SIGNED_UPDATE=OFF` | 只自己用，不需要 OpenSSL 依赖 |

**需要串流功能时**：用 `启动-作者完整版.cmd` 切到作者原版 exe，那个构建里都有。这也是 `.orig` 必须保留的原因。

---

## 五、部署与双版本切换

### 5.1 切换机制

用户在部署目录双击 `.cmd` 切换两个版本：

| 脚本 | 切到 | 数据目录 |
|---|---|---|
| `启动-个人定制版.cmd` | 定制版（`.custom` 系列）| `user-data-2.0.0` |
| `启动-作者完整版.cmd` | 作者原版（`.orig` 系列）| `user-data-2.0.0-author` |

**每个脚本做三件事**（用相对路径 `%~dp0`，换目录不用改）：

```bat
copy /y "veyra_qml_ui.exe.custom" "veyra_qml_ui.exe"                    rem 1/3 exe
robocopy "qml.custom\Veyra" "qml\Veyra" /MIR /NFL /NDL /NJH /NJS /NP    rem 2/3 QML
for %%f in ("shaders\*.dxil.custom") do copy /y "%%~f" "%%~df%%~pf%%~nf" rem 3/3 shader（全部）
start "" "veyra_qml_ui.exe" --data-dir "%~dp0runtime_local\user-data-2.0.0"
```

`/MIR` 是镜像，所以切换后目标目录**完全等于**源目录。

### 5.2 部署（改代码后）

```bat
@echo off
set dep=D:\Program\Veyra-2.2.0-NVIDIA-win64-portable
set bld=D:\Program\Veyra\build-release
copy /y "%bld%\veyra_qml_ui.exe" "%dep%\veyra_qml_ui.exe"
copy /y "%bld%\veyra_qml_ui.exe" "%dep%\veyra_qml_ui.exe.custom"
copy /y "%bld%\shaders\*.dxil" "%dep%\shaders\"
robocopy "%bld%\qml\Veyra" "%dep%\qml.custom\Veyra" /MIR /NFL /NDL /NJH /NJS /NP
robocopy "%bld%\qml\Veyra" "%dep%\qml\Veyra" /MIR /NFL /NDL /NJH /NJS /NP
```

**注意**：`shaders\*.dxil` 部署后，还要同步 `.dxil.custom` 副本，否则切换到定制版时会用旧 shader：

```bat
for %f in ("%dep%\shaders\*.dxil") do copy /y "%f" "%f.custom"
```

### 5.3 部署后必须报告

- 生效 exe 的 SHA256 与 build 产物**一致**
- `.custom` 与生效一致
- `.orig` **未被改动**（时间戳应保持作者发布时的时间）
- QML 数量一致
- 启动后日志无 `Unexpected token` / `no root object` / `Type ... unavailable`

### 5.4 作者发新版本时

```
作者新 exe      → veyra_qml_ui.exe.orig
作者新 shader   → shaders\*.dxil.orig（全部 72 个）
作者新 qml      → qml.orig\Veyra\（先清空再放）
```

然后用 `打包分享版.cmd` 里的排除逻辑检查一遍，确保 `.orig` 三样都齐。

---

## 六、验证方法

### 6.1 启动验证

```powershell
cd D:\Program\Veyra-2.2.0-NVIDIA-win64-portable
.\veyra_qml_ui.exe --data-dir "D:\Program\Veyra-2.2.0-NVIDIA-win64-portable\runtime_local\user-data-2.0.0"
```

看日志 `logs\veyra-qml.log`：

- 出现 `[app] Veyra QML session started` = 启动成功
- 无 `[crash] unhandled exception` = 没崩
- 无 QML 报错 = 界面正常

### 6.2 B 站功能验证（最常测）

```powershell
curl -s -X POST http://127.0.0.1:18765/play -H "Content-Type: application/json" -d '{\"url\":\"https://www.bilibili.com/video/BV1sKs16AEWv\",\"quality\":112}'
```

日志里应该出现：

```
[bilibili] resolving page URL (cookie: signed in, qn: 112, danmaku: true, subtitles: true, ...)
[bilibili] resolved 1920x1080 qn=112 audio=dash
[danmaku] loaded items=7xx
[subtitle] bilibili subtitle installed lan=both cues=7x
[audio] pipeline started separateAudio=1
```

### 6.3 测试环境变量必须干净

```powershell
python -c "import os;print([k for k in os.environ if k.startswith('VEYRA_TEST')])"
```

必须输出空列表。残留的测试变量会让程序行为异常（比如限速变量让本地视频被当网络源，首帧等 12 秒）。

---

## 七、回退预案

改动前确认能回到上一个验收过的版本。

| 方式 | 位置 |
|---|---|
| git | `git checkout <上一个验收过的提交>` |
| 部署备份 | `veyra_qml_ui.exe.orig`（作者原版，随时可切回）|
| 分享包 | `打包分享版.cmd` 生成的完整副本 |

**两条纪律**：

1. **切分支 / 换提交后必须全量重建**，不能增量（会得到"看着对、跑起来不对"的混合产物）
2. **exe 的哈希每次构建都会变**（MSVC 写入编译时间戳），不要拿哈希不等当"没回到旧版"的证据。判断标准是源码 `git status` 干净 + 部署 exe 与 build 产物字节相同

---

## 八、踩过的坑（每天都会遇到的）

1. **中文路径不能用于构建**。Ninja 生成不出 `rules.ninja`，报 `unknown build rule`。

2. **`VEYRA_BUILD_QML_UI=ON` 必须给**。默认 OFF，漏了就构建成功但没主程序。

3. **切分支/改配置后必须先清干净再编**。否则 `LNK4098` / `LNK2019`，最坏是运行期崩溃。

4. **测试环境变量不能残留**。用 `python -c "import os;print([k for k in os.environ if k.startswith('VEYRA_TEST')])"` 自查，必须为空。

5. **改链接选项（`/DEBUG`、`CMAKE_BUILD_TYPE`）后必须全量重建**。混用会产生 STL ABI 不一致，运行期 `std::format` 崩溃。

6. **`.orig` 永远不动**。这是切回作者原版的唯一途径。

7. **弹幕/字幕开关可能被程序重置**。发现功能"自己关了"，去设置里重新打开展。

---

## 九、新会话第一步（照做）

```powershell
# 1. 确认代码状态
cd D:\Program\Veyra\src
git branch --show-current        # 应为 local/2.2.0-merge 或当前工作分支
git log --oneline -3
git status --porcelain           # 应为空

# 2. 确认没有残留测试环境变量（必须为空）
python -c "import os;print([k for k in os.environ if k.startswith('VEYRA_TEST')])"

# 3. 确认构建环境健康
Test-Path D:\Program\Veyra\build-release\CMakeFiles\rules.ninja   # 应为 True

# 4. 增量构建（约 10 秒）
& "C:\Program Files\CMake\bin\cmake.exe" --build D:\Program\Veyra\build-release -j 8

# 5. 部署（见 5.2）

# 6. 启动验证（见 6.1）
```

**如果要做大改动**（合并上游、改构建配置、切分支）：先按 4.1 全量重新配置 + 全量构建，不要走增量。

---

## 十、用户偏好（必须遵守）

1. **所有思考和回复用简体中文**，代码注释也尽量中文。
2. **临时文件放工作区 `.dsh-tmp\` 下**，不散落到盘符根目录或用户目录。
3. **删除一律进回收站**，禁止 `rm` / `Remove-Item` / `del` / `rmdir`。
4. **推送前必须问用户**，用户验收前一律不 push。
5. **验证请求必须连 exe 一起部署**，否则界面和描述不一致。
6. **改了东西先自己验证再报告**，不要说"应该可以"。

---

## 十一、给下一位的提醒

- 这个项目**文档很多但大多是作者的**。只看 `MY_WORK.md` + `HANDOFF.md` 这两份就够了。
- 作者的 `docs/` 目录有 600+ 份文档，是历史积累。遇到具体技术问题可以查，但**不要试图整理或合并它们**（会冲突，而且作者每版都更新）。
- **卡住的时候先看 `MY_WORK.md` 的「已知限制」**，很多"问题"是刻意的或做不到的。
- 部署目录里两个 `.cmd` 是用户的核心用法，**不要改动它们的逻辑**，除非用户明确要求。
