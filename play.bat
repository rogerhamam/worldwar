@echo off
rem Runs the game. Prefers the prebuilt worldwar_client.exe shipped at the repo
rem root (assets\ and data\ sit right next to it, which is where the exe looks
rem first). Falls back to a locally built dev binary if you've built from source.
cd /d "%~dp0"
if exist "worldwar_client.exe" (
    start "" "worldwar_client.exe"
    goto :eof
)
if exist "game\build\client\worldwar_client.exe" (
    set PATH=C:\msys64\ucrt64\bin;%PATH%
    cd /d "%~dp0game\build\client"
    start "" "worldwar_client.exe"
    goto :eof
)
echo Could not find worldwar_client.exe -- run build.bat first.
pause
