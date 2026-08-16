@echo off
setlocal EnableExtensions EnableDelayedExpansion

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

where powershell.exe >nul 2>&1
if errorlevel 1 (
  echo ERROR: Windows PowerShell was not found.
  set "VCVR_EXIT=1"
  goto :finished
)

if not exist "%VCVR_SCRIPT%" (
  echo The complete source kit is not beside this BAT file.
  echo Downloading and unpacking it automatically...
  echo.

  set "VCVR_BOOT=%TEMP%\ViceCityVR-SourceKit-%RANDOM%-%RANDOM%"
  set "VCVR_ZIP=%TEMP%\ViceCityVR-SourceKit-%RANDOM%-%RANDOM%.zip"
  >"%VCVR_LOG%" echo Vice City VR source-kit bootstrap

  powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
    "$ErrorActionPreference='Stop'; try { [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/dubrovskiy-yevhen-stakelogic/vice-city-vr-quest/archive/refs/heads/master.zip' -OutFile $env:VCVR_ZIP; Expand-Archive -LiteralPath $env:VCVR_ZIP -DestinationPath $env:VCVR_BOOT -Force; Remove-Item -LiteralPath $env:VCVR_ZIP -Force } catch { Add-Content -LiteralPath $env:VCVR_LOG -Value ($_ | Out-String); Write-Error $_; exit 1 }"
  if errorlevel 1 (
    echo.
    echo ERROR: The complete source kit could not be downloaded.
    echo Check the internet connection and the diagnostic log, then retry.
    set "VCVR_EXIT=1"
    goto :finished
  )

  for /d %%D in ("!VCVR_BOOT!\*") do if exist "%%~fD\tools\build-and-install.ps1" set "VCVR_SCRIPT=%%~fD\tools\build-and-install.ps1"
  if not exist "!VCVR_SCRIPT!" (
    echo ERROR: The downloaded source kit is incomplete.
    echo Expected tools\build-and-install.ps1 was not found after extraction.
    set "VCVR_EXIT=1"
    goto :finished
  )

  echo Source kit ready.
  echo.
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
