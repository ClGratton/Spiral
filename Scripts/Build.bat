@echo off
powershell -ExecutionPolicy Bypass -NoProfile -File "%~dp0Build.ps1" %*
