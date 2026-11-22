@echo off
rem Rekadeck firmware installer for Windows. Double-click this file, or run it from a terminal with options
rem (flash.bat --help lists them). The work happens in tools\flash.ps1.
setlocal
if not exist "%~dp0tools\flash.ps1" (
  echo.
  echo Error: the tools folder isn't next to flash.bat, so Windows only took this one file out of the ZIP.
  echo Right-click the ZIP, choose Extract All, then double-click flash.bat in the folder that makes.
  echo.
  if "%~1"=="" pause
  exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\flash.ps1" %*
set "code=%errorlevel%"
rem Started with a double-click (no options): keep the window open so the result can be read
if "%~1"=="" pause
exit /b %code%
