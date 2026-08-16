@echo off
setlocal
title Vice City VR - Personal Quest Builder

echo.
echo ============================================================
echo   VICE CITY VR - PERSONAL QUEST BUILD AND INSTALL WIZARD
echo ============================================================
echo.
echo This builds a personal APK from source. No APK or GTA data is
echo included in this repository.
echo.

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build-and-install.ps1" %*
set "VCVR_EXIT=%ERRORLEVEL%"

echo.
if not "%VCVR_EXIT%"=="0" (
  echo FAILED. Read the error above; nothing was silently ignored.
) else (
  echo SUCCESS. The wizard completed all requested steps.
)
echo.
pause
exit /b %VCVR_EXIT%
