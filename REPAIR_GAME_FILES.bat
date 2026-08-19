@echo off
setlocal
title Vice City VR - Repair Menu Text and Frontend Files
cd /d "%~dp0"

echo ================================================================
echo  VICE CITY VR - REPAIR MENU TEXT AND FRONTEND FILES
echo ================================================================
echo.
echo Run this if the OPTIONS menus show entries like "FET_GFX missing"
echo instead of proper labels. It copies the port's own replacement
echo text and frontend files to an existing installation.
echo.
echo Saves, settings and models are not touched.
echo.

set "INSTALLER=%~dp0tools\install-game-files.ps1"
if not exist "%INSTALLER%" (
    echo ERROR: Required file is missing:
    echo   %INSTALLER%
    echo.
    echo Extract the complete source-kit ZIP before running this file.
    set "RESULT=1"
    goto :finish
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%INSTALLER%" %*
set "RESULT=%ERRORLEVEL%"

:finish
echo.
if not "%RESULT%"=="0" (
    echo FAILED. Read the error above; nothing was silently ignored.
) else (
    echo COMPLETE. Fully restart Vice City VR on the headset.
)
echo.
echo This window will remain open. Press any key when you are done reading it.
pause >nul
exit /b %RESULT%
