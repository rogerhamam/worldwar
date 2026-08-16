@echo off
:: Build WorldWarLauncher.exe from launcher.py using PyInstaller.

echo Building World War Launcher...

pip install pyinstaller -q 2>NUL

:: Wipe caches so the icon / bundled data are always re-embedded fresh.
if exist "build" rmdir /s /q "build"
if exist "dist"  rmdir /s /q "dist"
if exist "WorldWarLauncher.spec" del /q "WorldWarLauncher.spec"

set ICON_ARGS=
if exist "icon.ico" set ICON_ARGS=--icon "icon.ico" --add-data "icon.ico;."

pyinstaller --clean --noconfirm --onefile --noconsole ^
    --name WorldWarLauncher ^
    %ICON_ARGS% ^
    --add-data "bg_menu.png;." ^
    launcher.py

if exist "dist\WorldWarLauncher.exe" (
    echo.
    echo Success: dist\WorldWarLauncher.exe
    echo This is the only file you distribute to players.
) else (
    echo.
    echo ERROR: Build failed.
)
pause
