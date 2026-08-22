@echo off
rem Build LNPP Manager with MSVC Build Tools
setlocal

set VSWHERE="C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSROOT=%%i
if not defined VSROOT (
    echo [ERROR] Visual Studio Build Tools not found
    exit /b 1
)

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1

cd /d "%~dp0"
if not exist build mkdir build

rem compile the icon resource (app.rc -> build\app.res)
rc /nologo /fo"build\\app.res" app.rc
if errorlevel 1 (
    echo [ERROR] Resource compile failed
    exit /b 1
)

cl /nologo /O2 /EHsc /std:c++17 /W3 /utf-8 ^
    /Fo"build\\" /Fe"lnpp.exe" ^
    src\main.cpp src\common.cpp src\process.cpp src\manager.cpp ^
    /link user32.lib gdi32.lib shell32.lib comctl32.lib advapi32.lib ole32.lib comdlg32.lib ^
    build\\app.res

if errorlevel 1 (
    echo [ERROR] Build failed
    exit /b 1
)
echo [OK] lnpp.exe