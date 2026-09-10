@echo off
rem Build LNPP Manager with MSVC Build Tools
rem Usage: build.bat [debug]    (default: release)
rem   debug   - /Od /Zi /D_DEBUG /MTd, output lnpp_dbg.exe (for debugging
rem             the process.cpp UI-thread assertions etc.)
setlocal

set BUILD_MODE=release
if /i "%~1"=="debug" set BUILD_MODE=debug

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

rem compile the icon + version resource (app.rc -> build\app.res)
rc /nologo /c 65001 /fo"build\\app.res" app.rc
if errorlevel 1 (
    echo [ERROR] Resource compile failed
    exit /b 1
)

rem /MT: statically link the CRT so the exe runs without VC Redist
rem (portable tool). debug uses /MTd + /Zi for symbol files.
if "%BUILD_MODE%"=="debug" (
    set CL_FLAGS=/nologo /Od /EHsc /std:c++17 /W4 /permissive- /utf-8 /D_DEBUG /Zi /MTd
    set OUT_EXE=lnpp_dbg.exe
) else (
    set CL_FLAGS=/nologo /O2 /EHsc /std:c++17 /W4 /permissive- /utf-8 /MT
    set OUT_EXE=lnpp.exe
)

cl %CL_FLAGS% /Fo"build\\" /Fe"%OUT_EXE%" ^
    src\main.cpp src\common.cpp src\process.cpp src\manager.cpp src\downloader.cpp ^
    /link user32.lib gdi32.lib shell32.lib comctl32.lib advapi32.lib ole32.lib comdlg32.lib winhttp.lib version.lib crypt32.lib ^
    build\\app.res

if errorlevel 1 (
    echo [ERROR] Build failed
    echo         If the error above is LNK1104 about %OUT_EXE%, the program is
    echo         still running - exit it from the tray icon first.
    exit /b 1
)
echo [OK] %OUT_EXE%
