@echo off
setlocal

rem A fresh Windows install only has Windows PowerShell 5.1, which can neither parse setup.ps1
rem (PowerShell 7 syntax) nor run scripts at all under its default execution policy. So bootstrap
rem PowerShell 7.5+ from cmd, then hand over to setup.ps1.

set "PWSH=pwsh"

rem exits non-zero if pwsh is missing (cmd's 9009) or older than 7.5
pwsh -NoProfile -Command "exit [int]($PSVersionTable.PSVersion -lt [version]'7.5')" >nul 2>nul
if errorlevel 1 (
	echo PowerShell 7.5+ could not be found, installing PowerShell 7...

	where winget >nul 2>nul
	if errorlevel 1 (
		echo winget not found. Install or update 'App Installer' from the Microsoft Store: https://aka.ms/getwinget
		exit /b 1
	)

	rem installs, or upgrades an older version in place
	winget install --exact --silent --id Microsoft.PowerShell --source winget --accept-package-agreements --accept-source-agreements --disable-interactivity

	rem the install doesn't update the PATH of this process
	set "PWSH=%ProgramFiles%\PowerShell\7\pwsh.exe"
	if not exist "%ProgramFiles%\PowerShell\7\pwsh.exe" (
		echo PowerShell 7 installation failed
		exit /b 1
	)
)

"%PWSH%" -ExecutionPolicy Bypass -File "%~dp0setup.ps1" %*
exit /b %errorlevel%
