@echo off
:: Fallout 2: Narg Edition installer (Windows). Extract the archive into the
:: Fallout 2 folder, then run this file there. Options are passed on, e.g.
::   narg-install.bat -With no_wind,no_shot_light -Resolution 2560x1440
:: The work is done by narg\narg-install.ps1 (PowerShell, part of Windows).
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0narg\narg-install.ps1" %*
pause
