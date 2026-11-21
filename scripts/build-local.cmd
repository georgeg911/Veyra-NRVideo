@echo off
rem 增量构建（满核并行）。用法：build.cmd
rem 注意：不要把这个脚本的输出接管道（| grep / | head），
rem       head 提前退出会让上游 cmd 阻塞，批处理看起来"卡住"。
rem       要看错误就重定向到文件：build.cmd > build.log 2>&1
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo [错误] vcvars64.bat 失败 & exit /b 1 )
"C:\Program Files\CMake\bin\cmake.exe" --build D:\Program\Veyra\build-release -j 16
exit /b %errorlevel%
