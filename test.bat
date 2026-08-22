@echo off
rem Build & run LNPP test suite (selftest / proctest / migtest)
rem Usage: test.bat [build|run|all]    (default: all)
rem   build  - compile the three test executables into the project root
rem   run    - run existing test executables (selftest then proctest then migtest)
rem   all    - build then run
rem
rem NOTE: test exes must live in the project root (next to bin\ etc\) because
rem all paths are derived from the exe location.
setlocal

set MODE=%~1
if "%MODE%"=="" set MODE=all
if not "%MODE%"=="build" if not "%MODE%"=="run" if not "%MODE%"=="all" (
    echo [ERROR] unknown mode: %MODE%  ^(use build / run / all^)
    exit /b 1
)

set VSWHERE="C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSROOT=%%i
if not defined VSROOT (
    echo [ERROR] Visual Studio Build Tools not found
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 exit /b 1

cd /d "%~dp0"
if not exist build mkdir build

set LIBS=user32.lib gdi32.lib shell32.lib comctl32.lib advapi32.lib ole32.lib winhttp.lib version.lib
set FAILED=0

if not "%MODE%"=="run" (
    echo [BUILD] selftest.exe
    cl /nologo /O2 /EHsc /std:c++17 /W3 /utf-8 /Fo"build\\" /Fe"selftest.exe" ^
        src\selftest.cpp src\common.cpp src\process.cpp src\manager.cpp src\downloader.cpp /link %LIBS%
    if errorlevel 1 set FAILED=1
    echo [BUILD] proctest.exe
    cl /nologo /O2 /EHsc /std:c++17 /W3 /utf-8 /Fo"build\\" /Fe"proctest.exe" ^
        src\proctest.cpp src\common.cpp src\process.cpp src\manager.cpp src\downloader.cpp /link %LIBS%
    if errorlevel 1 set FAILED=1
    echo [BUILD] migtest.exe
    cl /nologo /O2 /EHsc /std:c++17 /W3 /utf-8 /Fo"build\\" /Fe"migtest.exe" ^
        src\migtest.cpp src\common.cpp src\process.cpp src\manager.cpp src\downloader.cpp /link %LIBS%
    if errorlevel 1 set FAILED=1
)

if "%MODE%"=="build" (
    if "%FAILED%"=="1" ( echo [ERROR] test build failed & exit /b 1 )
    echo [OK] tests built to project root
    exit /b 0
)

if not exist selftest.exe (
    echo [ERROR] selftest.exe missing - run "test.bat build" first
    exit /b 1
)

for %%T in (selftest proctest migtest) do (
    echo.
    echo ===== RUN %%T =====
    "%~dp0%%T.exe"
    if errorlevel 1 (
        echo [FAIL] %%T.exe exited nonzero
        set FAILED=1
    ) else (
        echo [PASS] %%T.exe
    )
)

if "%FAILED%"=="1" (
    echo [ERROR] some tests failed
    exit /b 1
)
echo [OK] all tests passed
exit /b 0
