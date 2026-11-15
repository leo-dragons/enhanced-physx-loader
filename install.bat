@echo off
rem Enhanced PhysX Loader for Mirror's Edge - by leo_dragons
rem Usage: install.bat                      (finds the game automatically)
rem        install.bat "C:\...\mirrors edge" (install into a specific folder)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\install.ps1" -GamePath "%~1"
