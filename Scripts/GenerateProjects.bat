@echo off
powershell -ExecutionPolicy Bypass -NoProfile -File "%~dp0GenerateProjects.ps1" %*
