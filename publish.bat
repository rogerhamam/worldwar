@echo off
setlocal enabledelayedexpansion

:: ================================================================
:: World War - publish a new release
:: 1. Builds worldwar_client.exe (game/)
:: 2. Assembles dist/WorldWar/ (exe + runtime DLLs + assets + data --
::    ONLY what the C++ client needs to run; no GameMaker scripts, no
::    engine/editor source code)
:: 3. Generates manifest.json with SHA-256 hashes
:: 4. Zips game files and uploads to GitHub Releases
::
:: Requires: gh (GitHub CLI) - https://cli.github.com
::           a bash on PATH (msys64\usr\bin or Git Bash) for the ldd-based
::           DLL sweep
:: Usage:    publish.bat v0.1
:: ================================================================

set "ROOT=%~dp0"
set "DIST=%ROOT%dist\WorldWar"
:: build-release, not build: `build` is configured for an MSYS2 ucrt64
:: toolchain that is not installed on this machine, so its cmake --build fails
:: and the release would either abort or ship whatever stale exe was left in
:: it. build-release is the WinLibs UCRT tree that build.bat's successor
:: actually maintains, and it builds with nothing extra on PATH (the CMake
:: cache stores absolute compiler/ninja paths).
set "BUILD=%ROOT%game\build-release"
set "REPO=KAJKINGDOM/worldwar-releases"

if "%~1"=="" (
    echo Usage: publish.bat v0.1
    exit /b 1
)
set "TAG=%~1"

set "NOTES_FILE=%ROOT%release_notes.txt"
if not exist "%NOTES_FILE%" (
    echo ERROR: release_notes.txt not found. Create it in the project root.
    exit /b 1
)

echo ============================================
echo  World War Publisher - %TAG%
echo ============================================
echo.

:: ---- Step 1: Build ----
echo [1/4] Building game...
cd /d "%BUILD%"
cmake --build . --target worldwar_client >NUL 2>&1
if errorlevel 1 (
    echo ERROR: Build failed.
    exit /b 1
)
echo       Done.

:: ---- Step 2: Assemble dist ----
echo [2/4] Assembling dist folder...
if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%"

copy "%BUILD%\client\worldwar_client.exe" "%DIST%\" >NUL

:: Runtime DLLs.
::
:: This used to shell out to `bash -c "ldd ... | awk ..."`. That is a silent
:: trap: bash is NOT on the system PATH (it lives in Git's usr\bin, which is
:: normally not exported), the line had no errorlevel check, and the script
:: carried on regardless -- so the release zip was assembled with NO runtime
:: DLLs at all and would not start on any machine. It bit us on v2.16.0.
::
:: Copied explicitly instead, from the same trees CMake links against, and
:: every one is verified below. Losing ldd's automatic discovery is worth it:
:: this list changes about once a year, and a missing DLL now stops the
:: release instead of shipping a dead build.
echo       Copying runtime DLLs...
set "EDEPS=C:\Users\hjosh\Desktop\WORLD WAR KAJ\worldwar_campaign_editor-master\worldwar_campaign_editor-master\deps"
set "CDEPS=C:\Users\hjosh\Desktop\WORLD WAR KAJ\worldwar_cpp-master\worldwar_cpp-master\deps"
set "TOOLCHAIN=C:\Users\hjosh\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
copy "%EDEPS%\SDL2-2.32.10\x86_64-w64-mingw32\bin\SDL2.dll"             "%DIST%\" >NUL 2>&1
copy "%EDEPS%\SDL2_image-2.8.12\x86_64-w64-mingw32\bin\SDL2_image.dll"  "%DIST%\" >NUL 2>&1
copy "%EDEPS%\SDL2_ttf-2.24.0\x86_64-w64-mingw32\bin\SDL2_ttf.dll"      "%DIST%\" >NUL 2>&1
copy "%CDEPS%\SDL2_mixer-2.8.2\x86_64-w64-mingw32\bin\SDL2_mixer.dll"   "%DIST%\" >NUL 2>&1
copy "%TOOLCHAIN%\libgcc_s_seh-1.dll"                                   "%DIST%\" >NUL 2>&1
copy "%TOOLCHAIN%\libstdc++-6.dll"                                      "%DIST%\" >NUL 2>&1
copy "%TOOLCHAIN%\libwinpthread-1.dll"                                  "%DIST%\" >NUL 2>&1

:: Verify. A release that starts for nobody is worse than no release.
for %%D in (SDL2.dll SDL2_image.dll SDL2_ttf.dll SDL2_mixer.dll libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll) do (
    if not exist "%DIST%\%%D" (
        echo ERROR: runtime DLL missing from dist: %%D
        echo        Check EDEPS/CDEPS/TOOLCHAIN at the top of this step.
        exit /b 1
    )
)

:: Assets/data are the repo's shared runtime dirs (images, sounds, fonts,
:: JSON catalogs). Deliberately not copying any engine/editor source --
:: none of that is needed to play the build, so none of it goes in the zip.
echo       Copying assets and data...
xcopy "%ROOT%assets" "%DIST%\assets\" /E /I /Q >NUL
xcopy "%ROOT%data" "%DIST%\data\" /E /I /Q >NUL
echo %TAG%> "%DIST%\version.txt"
echo       Done.

:: ---- Step 3: Generate manifest + zip ----
echo [3/4] Generating manifest and zip...
python "%ROOT%launcher\launcher.py" --manifest "%DIST%" "%ROOT%dist\manifest.json"
cd /d "%ROOT%dist"
powershell -Command "Compress-Archive -Path 'WorldWar\*' -DestinationPath 'worldwar-game.zip' -Force"
echo       Done.

:: ---- Step 4: Upload to GitHub ----
echo [4/4] Publishing to GitHub as %TAG%...
cd /d "%ROOT%"

gh release --repo %REPO% delete %TAG% --yes >NUL 2>&1
gh release --repo %REPO% create %TAG% --title "World War %TAG%" --notes-file "%NOTES_FILE%" --latest
if errorlevel 1 (
    echo.
    echo ERROR: gh release create failed. Stopping.
    exit /b 1
)
gh release --repo %REPO% upload %TAG% "dist\manifest.json" --clobber
if errorlevel 1 (
    echo ERROR: manifest upload failed.
    exit /b 1
)
gh release --repo %REPO% upload %TAG% "dist\worldwar-game.zip" --clobber
if errorlevel 1 (
    echo ERROR: game zip upload failed.
    exit /b 1
)

echo.
echo ============================================
echo  Published World War %TAG%
echo  Players will auto-update on next launch.
echo ============================================
pause
