@echo off
chcp 936 >nul
setlocal
cd /d "%~dp0"

echo ============================================================
echo   Veyra 2.2.0 - 打包分享版
echo   排除: 个人数据 / 播放历史 / HDR配置 / 日志 / 崩溃转储
echo   保留: 程序本体 / QML / shaders / 双版本备份 / AMD+Intel 运行时
echo ============================================================
echo.

tasklist /fi "imagename eq veyra_qml_ui.exe" 2>nul | find /i "veyra_qml_ui.exe" >nul
if not errorlevel 1 (
  echo [错误] Veyra 正在运行，请先完全退出再打包。
  pause
  exit /b 1
)

set "OUT=%~dp0..\Veyra-2.2.0-share"
if exist "%OUT%" (
  echo [提示] 目标目录已存在:
  echo        %OUT%
  echo        请先移走或删除它再重新运行。
  pause
  exit /b 1
)

echo 正在复制，请稍候（约 1.1 GB，含 NVIDIA/AMD/Intel 运行时）...
robocopy "%~dp0." "%OUT%" /E /NFL /NDL /NJH /NJS /NP ^
  /XD "%~dp0runtime_local\user-data-2.0.0" ^
      "%~dp0runtime_local\user-data-2.0.0-author" ^
      "%~dp0runtime_local\user-data-2.0.0-p5" ^
      "%~dp0logs" ^
  /XF "*.dmp" "*.log"

if errorlevel 8 (
  echo.
  echo [错误] 复制失败，请检查磁盘空间或权限。
  pause
  exit /b 1
)

echo.
echo 完成。分享目录:
echo   %OUT%
echo.
echo 说明: 朋友拿到后直接运行 启动-个人定制版.cmd。
echo       实时翻译资产不含在内，需要时在界面里点下载。
echo       首次启动会自动创建干净的个人数据目录。
echo.
pause
