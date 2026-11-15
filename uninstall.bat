@echo off
rem Enhanced PhysX Loader for Mirror's Edge - by leo_dragons
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\install.ps1" -GamePath "%~1" -Uninstall
