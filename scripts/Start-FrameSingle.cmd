@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-FrameSingle.ps1" %*
set "FrameStartupExitCode=%ERRORLEVEL%"
if not "%FrameStartupExitCode%"=="0" pause
exit /b %FrameStartupExitCode%
