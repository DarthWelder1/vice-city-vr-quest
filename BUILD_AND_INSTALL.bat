@echo off
setlocal

rem A double-click normally starts cmd.exe /c, which can disappear before a
rem user can read an early host/security error. Run the real wizard inside a
rem nested /k shell. That shell remains open even if PowerShell is terminated.
if not defined VCVR_KEEP_OPEN (
  set "VCVR_KEEP_OPEN=1"
  "%ComSpec%" /d /k call "%~f0" %*
  exit /b
)

title Vice City VR - Personal Quest Builder

echo.
echo ============================================================
echo   VICE CITY VR - PERSONAL QUEST BUILD AND INSTALL WIZARD
echo ============================================================
echo.
echo This builds a personal APK from source. No APK or GTA data is
echo included in this repository.
echo.

set "VCVR_SCRIPT=%~dp0tools\build-and-install.ps1"
set "VCVR_LOG=%TEMP%\ViceCityVR-Build-And-Install.log"

if not exist "%VCVR_SCRIPT%" (
  echo ERROR: Required file is missing:
  echo   %VCVR_SCRIPT%
  echo.
  echo Extract the complete source-kit ZIP before running this file.
  set "VCVR_EXIT=1"
  goto :finished
)

where powershell.exe >nul 2>&1
if errorlevel 1 (
  echo ERROR: Windows PowerShell was not found.
  set "VCVR_EXIT=1"
  goto :finished
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%VCVR_SCRIPT%" -LogPath "%VCVR_LOG%" %*
set "VCVR_EXIT=%ERRORLEVEL%"

:finished
echo.
if not "%VCVR_EXIT%"=="0" (
  echo FAILED. Read the error above; nothing was silently ignored.
) else (
  echo SUCCESS. The wizard completed all requested steps.
)
echo.
echo Diagnostic log:
echo   %VCVR_LOG%
echo.
echo This window will remain open. Press any key when you are done reading it.
pause >nul
exit /b %VCVR_EXIT%
