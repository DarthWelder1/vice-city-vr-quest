@echo off
setlocal

if /I "%~1"=="--help" goto :help
if /I "%~1"=="-h" goto :help

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0transfer-vr-save.ps1" -Mode Import %*
exit /b %ERRORLEVEL%

:help
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0transfer-vr-save.ps1" -Help
exit /b %ERRORLEVEL%
