@echo off
REM SimpleIME incremental build wrapper - MUST run under vcvars64 env
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d D:\Documents\Hermes\WorkSpace\simpleIME
cmake --build build\RelWithDebInfo-clangcl-ninja-vcpkg --config RelWithDebInfo 2>&1
exit /b %ERRORLEVEL%