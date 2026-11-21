@echo off
chcp 936 >nul
setlocal
cd /d "%~dp0"

rem ============================================================
rem  个人定制版（本仓库构建）
rem ============================================================
rem  B 站播放（粘 URL 即播）/ 弹幕 / 双语字幕 / 杜比视界高光增强

rem --- 启动前预检 ---
rem find 用绝对路径：PATH 里有 Git/MSYS 的 find.exe 时会抢走它，报 "find: /i"
tasklist /fi "imagename eq veyra_qml_ui.exe" 2>nul | "%SystemRoot%\System32\find.exe" /i "veyra_qml_ui.exe" >nul
if not errorlevel 1 (
  echo [错误] Veyra 正在运行。请先完全退出，再运行本脚本。
  echo        切换只替换 exe 和界面文件，程序运行时替换会导致版本不一致。
  pause
  exit /b 1
)

if not exist "veyra_qml_ui.exe.custom" (
  echo [错误] 找不到 veyra_qml_ui.exe.custom，无法切换到定制版。
  pause
  exit /b 1
)
if not exist "qml.custom\Veyra" (
  echo [错误] 找不到 qml.custom\Veyra 文件夹，无法切换到定制版。
  pause
  exit /b 1
)

copy /y "veyra_qml_ui.exe.custom" "veyra_qml_ui.exe" >nul 2>nul
if errorlevel 1 (
  echo [错误] exe 替换失败。请完全退出 Veyra 后重试。
  pause
  exit /b 1
)

robocopy "qml.custom\Veyra" "qml\Veyra" /MIR /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 (
  echo [错误] 界面文件同步失败。请完全退出 Veyra 后重试。
  pause
  exit /b 1
)

for %%f in ("shaders\*.dxil.custom") do if exist "%%~f" copy /y "%%~f" "%%~df%%~pf%%~nf" >nul 2>nul

echo 已切换到 个人定制版。
echo   * B 站播放：粘贴视频页 URL 即可，带弹幕与中英双语字幕
echo   * 杜比视界高光增强（设置里可开关）
echo   * Chrome 插件一键拉起播放
echo 正在启动 Veyra ...
start "" "veyra_qml_ui.exe" --data-dir "%~dp0runtime_local\user-data-2.0.0" %*
rem exit（不带 /b）才会关闭这个 cmd 窗口
exit
