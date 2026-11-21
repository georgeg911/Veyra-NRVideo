# 部署目录的启动脚本（源）

这些是 **部署目录**（`D:\Program\Veyra-2.2.0-NVIDIA-win64-portable\`）里那四个 `.cmd` 的备份。
它们不在构建产物里，所以单独存一份，避免重新打包/迁移时丢失。

| 文件 | 作用 |
|---|---|
| `启动-个人定制版.cmd` | 切到定制版（`.custom` 三件套），数据目录 `user-data-2.0.0` |
| `启动-作者完整版.cmd` | 切到作者原版（`.orig` 三件套），数据目录 `user-data-2.0.0-author` |
| `打包分享版.cmd` | 生成可分享副本，排除个人数据/日志/翻译资产 |
| `install-protocol.cmd` | 注册 `veyra://` 协议（写 HKCU） |

## 两个必须注意的地方

**1. `find` 必须用绝对路径**

```bat
"%SystemRoot%\System32\find.exe" /i "veyra_qml_ui.exe"
```

PATH 里如果有 Git/MSYS 的 `find.exe`，它会抢走调用并报 `find: '/i': No such file or directory`
（Unix find 不认识 `/i`），导致"程序是否在运行"的预检失效。

**2. 结尾用 `exit` 而不是 `exit /b 0`**

`exit /b` 只退出批处理，cmd 窗口留在原地。`exit` 才会关窗口。

## 文件放在部署目录的哪里

直接放在部署目录根（和 `veyra_qml_ui.exe` 同级），脚本内全部用 `%~dp0` 相对路径。
