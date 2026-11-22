# Building the personal customization

**English** · [中文](#中文)

This document describes how this branch (`custom/2.2.0`) was built. It is written for
reproducibility — if the original machine is gone, this is what you need to rebuild it.

> This is a **personal fork**, not an upstream branch. See [PERSONAL-FORK.md](PERSONAL-FORK.md)
> for what it changes.

---

## 1. Prerequisites

Versions this branch was built and tested against. Other versions will probably work, but
these are known-good.

| Component | Version | Notes |
|---|---|---|
| Windows | 10 / 11 x64 | |
| Visual Studio | 2022 Community | MSVC v143 toolset |
| CMake | 3.25+ | |
| Ninja | bundled with VS | |
| Qt | **6.8.3** msvc2022_64 | QML UI depends on it |
| FFmpeg | **n9.0.1** lgpl-shared | prebuilt shared libraries |
| libdovi | in-tree at `deps/libdovi` | Dolby Vision RPU parsing |

### Vendor SDKs (not in this repository)

These live under `third_party_local/`, which `.gitignore` excludes. The build needs them
only for the features listed; drop the corresponding `-DVEYRA_*_ROOT` flag to build without.

| SDK | Feature it enables |
|---|---|
| DLSS SDK 310.7 | DLSS / Ray Reconstruction |
| nv-codec-headers 13.0 | NVENC export |
| XeSS 3.0.2 | Intel frame generation |
| AMF SDK 1.5.3 | AMD encoder |
| FidelityFX SDK (1.x and 2.3.0) | FSR |
| Optical Flow SDK 5.0.7 | NVIDIA VFG (frame interpolation) |
| `nvngx_dlssnr.dll` | DLSS NR (`VEYRA_ENABLE_EXPERIMENTAL_DLSSNR`) |

---

## 2. Configure

Run from an **ASCII-only path**. Non-ASCII characters in the source or build path break the
MSVC toolchain in ways that are painful to diagnose.

```bat
call "<VS>\VC\Auxiliary\Build\vcvars64.bat"
cmake -S <src> -B <build> -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH="<Qt 6.8.3 msvc2022_64>" ^
  -DVEYRA_FFMPEG_ROOT=<ffmpeg shared root> ^
  -DVEYRA_LIBDOVI_ROOT=<src>/deps/libdovi ^
  -DVEYRA_ENABLE_LIBDOVI=ON ^
  -DVEYRA_BUILD_QML_UI=ON ^
  -DVEYRA_ENABLE_SIGNED_UPDATE=OFF ^
  -DVEYRA_ENABLE_EXPERIMENTAL_DLSSNR=ON ^
  -DVEYRA_ENABLE_D3D12_DEBUG=ON ^
  -DVEYRA_ENABLE_DEINTERLACE=ON ^
  -DVEYRA_DLSS_SDK_ROOT=<src>/third_party_local/nvidia/DLSS-SDK-310.7 ^
  -DVEYRA_NVENC_HEADERS_ROOT=<src>/third_party_local/nvidia/nv-codec-headers-13.0 ^
  -DVEYRA_XESS_ROOT=<src>/third_party_local/intel/xess-3.0.2 ^
  -DVEYRA_AMF_SDK_ROOT=<src>/third_party_local/amd/AMF-SDK-1.5.3 ^
  -DVEYRA_FIDELITYFX_ROOT=<src>/third_party_local/amd/FidelityFX-SDK/sdk ^
  -DVEYRA_FIDELITYFX2_ROOT=<src>/third_party_local/amd/FidelityFX-SDK-2.3.0/Kits/FidelityFX ^
  -DVEYRA_CUDA_DRIVER_INCLUDE_DIR=<src>/third_party_local/nvidia/Optical_Flow_SDK_5.0.7/NvOFFRUC/NvOFFRUCSample/cudart_header ^
  -DVEYRA_DISPLAY_VERSION=2.2.0
```

### Three flags you must not omit

| Flag | What breaks without it |
|---|---|
| `VEYRA_BUILD_QML_UI=ON` | **No `veyra_qml_ui.exe` is produced.** Default is OFF; the build succeeds and you have no player. |
| `VEYRA_ENABLE_EXPERIMENTAL_DLSSNR=ON` | The `veyra_ngx` target is not generated and **linking fails**. |
| `VEYRA_CUDA_DRIVER_INCLUDE_DIR=...` | **VFG is silently disabled.** The build succeeds with no warning, and NVIDIA VFG stays greyed out forever. |

The third one is the dangerous one: no error, no warning. Verify it took effect by checking
that `build.ninja` contains `VEYRA_HAVE_CUDA_DRIVER_HEADERS=1`.

---

## 3. Build

```bat
cmake --build <build> -j 16
```

**Do not pipe the build output.** Piping (`| tee`, `| more`) can deadlock the compiler
driver on Windows and leave you with a half-finished build that looks successful.

Redirect to a file instead, then read it:

```bat
cmake --build <build> -j 16 > build.log 2>&1
```

### Rebuilding after a header change

Changing a widely included header (`PlaylistModel.h`, `QmlPlayerBridge_p.h`,
`NetworkSettings.h`, `FramePacket.h`, `VideoPresenter.h`, `EngineController.h`,
`IFrameSource.h`) needs a **full rebuild**, not an incremental one. Incremental builds after
those changes produce a binary with mismatched object layouts, which crashes at startup
(typically `0xC0000005` at a low offset inside the executable).

Recycle the object files and build again:

```powershell
Get-ChildItem <build> -Recurse -Filter *.obj -File | ForEach-Object {
    [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile(
        $_.FullName,
        [Microsoft.VisualBasic.FileIO.UIOption]::OnlyErrorDialogs,
        [Microsoft.VisualBasic.FileIO.RecycleOption]::SendToRecycleBin)
}
```

---

## 4. Tests

```bat
ctest --test-dir <build> --output-on-failure
```

Ten suites. The playlist suite needs a writable fixture directory as `argv[1]`; without it
the test exits immediately with code 2 and prints nothing, which looks like a pass if you
only check the exit code of the wrapper script.

---

## 5. Deployment layout

The player is deployed as a portable directory:

```
Veyra-2.2.0-NVIDIA-win64-portable/
  veyra_qml_ui.exe            <- the customization
  veyra_qml_ui.exe.custom     <- byte-identical copy, used by the launcher
  veyra_qml_ui.exe.orig       <- author's original binary, never modified
  qml/Veyra/                  <- QML from this branch
  qml.custom/Veyra/           <- same files, used when the launcher switches
  runtime_local/user-data-2.0.0/   <- user data: playlist, login, preferences
```

Both QML copies and both executables must be updated together. Updating only one leaves the
launcher pointing at a stale binary, and the UI you see will not match the code you changed.

---

## 中文

本文档说明分支 `custom/2.2.0` 是怎么构建出来的，目的是**可复现**——如果原机器没了，
照这个能重新构建出来。

> 这是**个人定制分支**，不是上游分支。改了什么见 [PERSONAL-FORK.md](PERSONAL-FORK.md)。

### 1. 环境要求

这些是本分支构建和测试时用的版本，其他版本大概率也能用，但这些是已验证的。

| 组件 | 版本 | 说明 |
|---|---|---|
| Windows | 10 / 11 x64 | |
| Visual Studio | 2022 Community | MSVC v143 工具集 |
| CMake | 3.25+ | |
| Ninja | VS 自带 | |
| Qt | **6.8.3** msvc2022_64 | QML 界面依赖它 |
| FFmpeg | **n9.0.1** lgpl-shared | 预编译共享库 |
| libdovi | 仓库内 `deps/libdovi` | 杜比视界 RPU 解析 |

**第三方 SDK 不在仓库里**，位于 `third_party_local/`（被 `.gitignore` 排除）。不装某个
SDK 就把对应的 `-DVEYRA_*_ROOT` 去掉。

### 2. 配置

**构建路径必须是纯 ASCII**，含中文的路径会让 MSVC 工具链出各种难查的问题。

三个**绝对不能漏**的开关：

| 参数 | 漏了的后果 |
|---|---|
| `VEYRA_BUILD_QML_UI=ON` | **不生成 `veyra_qml_ui.exe`**，构建成功但没有主程序 |
| `VEYRA_ENABLE_EXPERIMENTAL_DLSSNR=ON` | `veyra_ngx` 目标不生成，**链接直接失败** |
| `VEYRA_CUDA_DRIVER_INCLUDE_DIR=…` | **VFG 静默禁用**，不报错不警告，补帧选项永远灰的 |

第三个最阴险。验证方法：配置后检查 `build.ninja` 里有 `VEYRA_HAVE_CUDA_DRIVER_HEADERS=1`。

### 3. 构建

```bat
cmake --build <build> -j 16
```

**绝对不要把构建输出接管道**（`| tee`、`| more`），Windows 上可能死锁编译器，留下一个
看起来成功实际半成品的结果。重定向到文件再读。

**改过广泛包含的头文件后必须全量重建**（回收 `.obj`），否则会得到目标文件布局不一致的
二进制，启动时崩在 `0xC0000005` 低偏移处。

### 4. 测试

十个套件。播放列表测试需要一个可写的 fixture 目录作为 `argv[1]`，不给的话测试立刻
以退出码 2 结束且没有任何输出——如果只看包装脚本的退出码，会误以为是通过的。

### 5. 部署结构

便携目录，两个 exe、两处 QML 必须同时更新。只更新一个会导致启动脚本指向旧二进制，
看到的界面和改过的代码对不上。
