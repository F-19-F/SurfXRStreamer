@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-Frame.ps1" %*
set "FrameInstallExitCode=%ERRORLEVEL%"
pause
exit /b %FrameInstallExitCode%
