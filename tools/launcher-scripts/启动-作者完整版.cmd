@echo off
chcp 936 >nul
setlocal
cd /d "%~dp0"

rem ============================================================
rem  作者完整版（作者发行包原样）
rem ============================================================

rem find 用绝对路径：PATH 里有 Git/MSYS 的 find.exe 时会抢走它，报 "find: /i"
tasklist /fi "imagename eq veyra_qml_ui.exe" 2>nul | "%SystemRoot%\System32\find.exe" /i "veyra_qml_ui.exe" >nul
if not errorlevel 1 (
  echo [错误] Veyra 正在运行。请先完全退出，再运行本脚本。
  pause
  exit /b 1
)

if not exist "veyra_qml_ui.exe.orig" (
  echo [错误] 找不到 veyra_qml_ui.exe.orig，无法切换到作者版。
  pause
  exit /b 1
)
if not exist "qml.orig\Veyra" (
  echo [错误] 找不到 qml.orig\Veyra 文件夹，无法切换到作者版。
  pause
  exit /b 1
)

copy /y "veyra_qml_ui.exe.orig" "veyra_qml_ui.exe" >nul 2>nul
if errorlevel 1 (
  echo [错误] exe 替换失败。请完全退出 Veyra 后重试。
  pause
  exit /b 1
)

robocopy "qml.orig\Veyra" "qml\Veyra" /MIR /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 (
  echo [错误] 界面文件同步失败。请完全退出 Veyra 后重试。
  pause
  exit /b 1
)

for %%f in ("shaders\*.dxil.orig") do if exist "%%~f" copy /y "%%~f" "%%~df%%~pf%%~nf" >nul 2>nul

echo 已切换到 作者完整版。
echo   * 纯作者 2.2.0，无本仓库定制
echo   * 串流/采集卡功能在这个版本里可用
echo 正在启动 Veyra ...
start "" "veyra_qml_ui.exe" --data-dir "%~dp0runtime_local\user-data-2.0.0-author" %*
rem exit（不带 /b）才会关闭这个 cmd 窗口
exit
