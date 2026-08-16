@echo off
set PATH=C:\msys64\ucrt64\bin;%PATH%
cd /d "%~dp0game\build\client"
start "" "worldwar_client.exe"
