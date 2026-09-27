@echo off
rem Release packaging: stage lnpp.exe + the required files, then zip them to
rem dist\LNPP-[version].zip
rem
rem Usage:
rem   package.bat              build if needed, then package (version from app.rc)
rem   package.bat 1.5.3        explicit version (does not touch app.rc)
rem   package.bat /nobuild     skip the build, use the existing lnpp.exe
rem
rem NOTE: this file is deliberately ASCII-only. cmd.exe decodes a .bat using the
rem console code page, so UTF-8 Chinese comments become mojibake that can
rem break the parser - and a mangled line here can turn a quoted path into a
rem destructive command. build.bat and test.bat are ASCII for the same reason.
rem
rem Packaging is a whitelist copy. bin\ data\ logs\ backup\ ssl\ and the user's
rem per-site nginx configs are never staged - that is where the private keys,
rem the database dumps, the DPAPI ciphertext and the developer paths live.
rem Step 3 re-checks the staged tree before anything is zipped.
setlocal enabledelayedexpansion

cd /d "%~dp0"

set NOBUILD=0
if /i "%~1"=="/nobuild" (
    set NOBUILD=1
    shift
)
set VER=%~1

rem ---- version: first three components of FILEVERSION in app.rc ----
if not defined VER (
    set FVC=
    for /f "tokens=1-4 delims=, " %%a in ('findstr /C:"FILEVERSION" app.rc 2^>nul') do (
        if not defined FVC set FVC=%%b.%%c.%%d
    )
    if not defined FVC (
        echo [ERROR] cannot read FILEVERSION from app.rc
        exit /b 1
    )
    set VER=!FVC!
)

echo ============================================================
echo  LNPP release package - version !VER!
echo ============================================================

rem Safety, before anything destructive runs: pin the staging path to
rem (repo)\dist\LNPP-[ver] and refuse to continue if the path does not end in
rem exactly that. An empty or mangled variable reaching the rmdir below is
rem how a stray rmdir once wiped the repository root.
set STAGE=%CD%\dist\LNPP-!VER!

rem ---- 1. build ----
if "%NOBUILD%"=="0" (
    echo [1/4] building lnpp.exe ...
    call build.bat
    if errorlevel 1 (
        echo [ERROR] build failed. If the message is LNK1104, exit LNPP from
        echo         the tray icon first, then run this again.
        exit /b 1
    )
) else (
    echo [1/4] skipping build - /nobuild
)

if not exist lnpp.exe (
    echo [ERROR] lnpp.exe not found
    exit /b 1
)

rem ---- 2. stage the whitelist ----
echo [2/4] staging files ...
if exist "%STAGE%\" (
    if not exist "%STAGE%\.lnpp-stage" (
        echo [ERROR] !STAGE! exists but carries no .lnpp-stage marker, so it was
        echo         not created by this script. Refusing to delete it.
        exit /b 1
    )
    rmdir /s /q "%STAGE%\"
)
mkdir "%STAGE%\etc\nginx\vhosts" 2>nul
mkdir "%STAGE%\etc\postgresql" 2>nul
mkdir "%STAGE%\etc\redis" 2>nul
mkdir "%STAGE%\www" 2>nul
> "%STAGE%\.lnpp-stage" echo created by package.bat

copy /y lnpp.exe                    "%STAGE%\" >nul || goto :copyfail
copy /y README.md                   "%STAGE%\" >nul || goto :copyfail
copy /y packages.conf.example       "%STAGE%\" >nul || goto :copyfail
if exist www\.gitkeep copy /y www\.gitkeep "%STAGE%\www\" >nul

rem etc: copy the five templates one by one, so a recursive copy can never
rem sweep in the user's own vhost .conf files.
copy /y etc\nginx\nginx.conf.tpl               "%STAGE%\etc\nginx\"         >nul 2>&1
copy /y etc\nginx\vhosts\_template.conf       "%STAGE%\etc\nginx\vhosts\" >nul 2>&1
copy /y etc\nginx\vhosts\_template_https.conf "%STAGE%\etc\nginx\vhosts\" >nul 2>&1
copy /y etc\postgresql\postgresql.conf.append "%STAGE%\etc\postgresql\"    >nul 2>&1
copy /y etc\redis\redis.conf.tpl               "%STAGE%\etc\redis\"         >nul 2>&1

rem ---- 3. refuse to ship anything sensitive ----
echo [3/4] verifying contents ...
set LEAK=
for %%D in (bin data logs backup ssl docs src scripts dist) do (
    if exist "%STAGE%\%%D" set LEAK=!LEAK! %%D/
)
if exist "%STAGE%\packages.conf" set LEAK=!LEAK!packages.conf
if exist "%STAGE%\app.manifest"  set LEAK=!LEAK!app.manifest
if exist "%STAGE%\app.rc"        set LEAK=!LEAK!app.rc
if exist "%STAGE%\build.bat"     set LEAK=!LEAK!build.bat
if exist "%STAGE%\test.bat"      set LEAK=!LEAK!test.bat
for %%F in ("%STAGE%\etc\nginx\vhosts\*") do (
    echo %%~nxF | findstr /b /i /c:"_template" >nul || set LEAK=!LEAK!etc\nginx\vhosts\%%~nxF
)
if defined LEAK (
    echo [ERROR] files that must never ship are in the staging tree: !LEAK!
    echo         Aborting. Check the whitelist in package.bat.
    rmdir /s /q "%STAGE%" 2>nul
    exit /b 1
)

rem ---- 4. zip ----
rem scratch tree: keep the .lnpp-stage marker until the zip is done, then
rem remove the whole staging dir so the next run starts clean.
echo [4/4] compressing ...
if not exist dist mkdir dist 2>nul
set ZIP=%CD%\dist\LNPP-!VER!.zip
if exist "%ZIP%" del /q "%ZIP%"
del /q "%STAGE%\.lnpp-stage" 2>nul
powershell -NoProfile -Command "Compress-Archive -LiteralPath '%STAGE%' -DestinationPath '%ZIP%' -Force"
if errorlevel 1 (
    echo [ERROR] compression failed
    exit /b 1
)

rmdir /s /q "%STAGE%\" 2>nul
for %%F in ("%ZIP%") do set ZSIZE=%%~zF
set /a ZKB=!ZSIZE! / 1024

echo.
echo ============================================================
echo  [OK] dist\LNPP-!VER!.zip   (!ZKB! KB)
echo ============================================================
echo contents:
for /f "delims=" %%F in ('dir /b /s "%STAGE%"') do (
    set REL=%%F
    set REL=!REL:%STAGE%\=!
    echo     !REL!
)
echo.
echo deliberately excluded: bin\ data\ logs\ backup\ ssl\ site configs
echo first run opens the component downloader; copy packages.conf.example to
echo packages.conf and fill in https URLs to use it.
exit /b 0

:copyfail
echo [ERROR] failed to copy a required file
exit /b 1
