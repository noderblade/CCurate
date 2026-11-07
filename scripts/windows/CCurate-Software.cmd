@echo off
setlocal
set "CCURATE_RENDERER=software"
start "" /wait "%~dp0ccurate.exe" %*
exit /b %errorlevel%
