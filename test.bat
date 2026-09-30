@echo off
rem Build & run the LNPP test suite.
rem
rem Usage: test.bat [build^|run^|all^|selftest^|proctest^|migtest^|destructive]  (default: all)
rem   build         - compile the four test executables into the project root
rem   run / all     - run unittests.exe ONLY (pure logic, no side effects)
rem   selftest      - run selftest.exe  (DESTRUCTIVE: starts/stops real components)
rem   proctest      - run proctest.exe  (DESTRUCTIVE: resurrects then pm2-kills apps)
rem   migtest       - run migtest.exe   (DESTRUCTIVE: migrates the real database)
rem   destructive   - run all three integration tests
rem
rem The integration tests drive the REAL stack: they start and stop real servers,
rem switch the active component versions, and migtest rebuilds the live
rem PostgreSQL cluster (pg_dumpall -^> initdb -^> restore). That is why they are
rem NOT part of the default run - `test.bat all` on a machine with real data used
rem to rebuild the production database and stop every pm2 app.
rem
rem Exit codes: 0 = pass, 1 = fail, 2 = skipped (SKIP is never reported as PASS).
rem
rem NOTE: test exes must live in the project root (next to bin\ etc\) because
rem all paths are derived from the exe location.
setlocal enabledelayedexpansion

set MODE=%~1
if "%MODE%"=="" set MODE=all
if /i "%MODE%"=="run" set MODE=all

rem SAFE_RUNS / DESTRUCTIVE_RUNS: empty means "skip the build", "!" is a dummy
rem entry so an empty list does not produce a malformed for-loop.
set SAFE_RUNS=
set DESTRUCTIVE_RUNS=
set DO_BUILD=0

if /i "%MODE%"=="all" set SAFE_RUNS=unittests
if /i "%MODE%"=="all" set DO_BUILD=1
if /i "%MODE%"=="build" set DO_BUILD=1
if /i "%MODE%"=="build" set SAFE_RUNS=__none__
if /i "%MODE%"=="destructive" set DESTRUCTIVE_RUNS=selftest proctest migtest
if /i "%MODE%"=="destructive" set DO_BUILD=1
if /i "%MODE%"=="selftest" set DESTRUCTIVE_RUNS=selftest
if /i "%MODE%"=="selftest" set DO_BUILD=1
if /i "%MODE%"=="proctest" set DESTRUCTIVE_RUNS=proctest
if /i "%MODE%"=="proctest" set DO_BUILD=1
if /i "%MODE%"=="migtest" set DESTRUCTIVE_RUNS=migtest
if /i "%MODE%"=="migtest" set DO_BUILD=1

if not defined SAFE_RUNS if not defined DESTRUCTIVE_RUNS (
    echo [ERROR] unknown mode: %MODE%
    echo         use build / run / all / selftest / proctest / migtest / destructive
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

set LIBS=user32.lib gdi32.lib shell32.lib comctl32.lib advapi32.lib ole32.lib winhttp.lib version.lib crypt32.lib ws2_32.lib
set FAILED=0

if "%DO_BUILD%"=="1" (
    echo [BUILD] unittests.exe
    cl /nologo /O2 /EHsc /std:c++17 /W4 /permissive- /utf-8 /MT /Fo"build\\" /Fe"unittests.exe" ^
        src\unittests.cpp src\common.cpp src\process.cpp src\manager.cpp src\downloader.cpp /link %LIBS%
    if errorlevel 1 set FAILED=1
    echo [BUILD] selftest.exe
    cl /nologo /O2 /EHsc /std:c++17 /W4 /permissive- /utf-8 /MT /Fo"build\\" /Fe"selftest.exe" ^
        src\selftest.cpp src\common.cpp src\process.cpp src\manager.cpp src\downloader.cpp /link %LIBS%
    if errorlevel 1 set FAILED=1
    echo [BUILD] proctest.exe
    cl /nologo /O2 /EHsc /std:c++17 /W4 /permissive- /utf-8 /MT /Fo"build\\" /Fe"proctest.exe" ^
        src\proctest.cpp src\common.cpp src\process.cpp src\manager.cpp src\downloader.cpp /link %LIBS%
    if errorlevel 1 set FAILED=1
    echo [BUILD] migtest.exe
    cl /nologo /O2 /EHsc /std:c++17 /W4 /permissive- /utf-8 /MT /Fo"build\\" /Fe"migtest.exe" ^
        src\migtest.cpp src\common.cpp src\process.cpp src\manager.cpp src\downloader.cpp /link %LIBS%
    if errorlevel 1 set FAILED=1
)

if /i "%MODE%"=="build" goto :build_done
if "%FAILED%"=="1" goto :build_failed
goto :run_tests

:build_done
if "%FAILED%"=="1" goto :build_failed
echo [OK] tests built to project root
exit /b 0

:build_failed
echo [ERROR] test build failed
exit /b 1

:run_tests
if not exist unittests.exe (
    echo [ERROR] unittests.exe missing - run "test.bat build" first
    exit /b 1
)

if defined DESTRUCTIVE_RUNS (
    if not "!DESTRUCTIVE_RUNS!"=="__none__" (
        echo.
        echo ============================================================
        echo  WARNING: destructive integration tests
        echo   These drive the REAL stack under this directory:
        echo     - components are really started and stopped
        echo     - the active component version is switched
        echo     - migtest rebuilds the live PostgreSQL cluster
        echo   Back up anything you care about first.
        echo ============================================================
    )
)

if defined SAFE_RUNS if not "!SAFE_RUNS!"=="__none__" for %%T in (%SAFE_RUNS%) do call :run_one %%T
if defined DESTRUCTIVE_RUNS for %%T in (%DESTRUCTIVE_RUNS%) do call :run_one %%T

if "%FAILED%"=="1" (
    echo [ERROR] some tests failed
    exit /b 1
)
echo [OK] all requested tests passed
exit /b 0

:run_one
echo.
echo ===== RUN %1 =====
"%~dp0%1.exe"
set RC=!errorlevel!
if "!RC!"=="0" echo [PASS] %1.exe
if "!RC!"=="2" echo [SKIP] %1.exe did not apply - see output above
if "!RC!"=="0" goto :run_one_done
if "!RC!"=="2" goto :run_one_done
echo [FAIL] %1.exe exited with !RC!
set FAILED=1
:run_one_done
exit /b 0
