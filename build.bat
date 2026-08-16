@echo off
setlocal enabledelayedexpansion

:: ================================================================
:: World War - build everything
:: Configures + builds both the main game (game/) and the campaign
:: editor (editor/) with CMake + Ninja.
:: Requires: MSYS2 ucrt64 toolchain (cmake, ninja, gcc) on PATH,
::           with SDL2, SDL2_image, SDL2_mixer, SDL2_ttf and
::           nlohmann_json installed (pacman/vcpkg).
:: ================================================================

set "ROOT=%~dp0"
set PATH=C:\msys64\ucrt64\bin;%PATH%

echo ============================================
echo  World War - building main game (game/)
echo ============================================
cmake -G Ninja -B "%ROOT%game\build" "%ROOT%game"
if errorlevel 1 goto :error
cmake --build "%ROOT%game\build"
if errorlevel 1 goto :error

echo.
echo ============================================
echo  World War - building campaign editor (editor/)
echo ============================================
cmake -G Ninja -B "%ROOT%editor\build" "%ROOT%editor"
if errorlevel 1 goto :error
cmake --build "%ROOT%editor\build"
if errorlevel 1 goto :error

echo.
echo ============================================
echo  Build complete.
echo  Game:            game\build\client\worldwar_client.exe
echo  Campaign editor: editor\build\campaign_editor.exe
echo ============================================
exit /b 0

:error
echo.
echo ERROR: build failed.
exit /b 1
