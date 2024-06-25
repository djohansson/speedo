. $PSScriptRoot/scripts/env.ps1
. $PSScriptRoot/scripts/platform.ps1
. $PSScriptRoot/scripts/vcpkg.ps1

$global:myEnv = New-Object -TypeName PSObject

$myEnvFile = "$PSScriptRoot/.env.json"

if (Test-Path $myEnvFile)
{
	$global:myEnv = Get-Content -Path $myEnvFile -Raw | ConvertFrom-Json
}

if ($IsWindows)
{
	& $PSScriptRoot/scripts/platforms/windows/windows.ps1
}
elseif ($IsMacOS)
{
	& $PSScriptRoot/scripts/platforms/osx/osx.ps1
}
elseif ($IsLinux)
{
	& $PSScriptRoot/scripts/platforms/linux/linux.ps1
}
else
{
	Write-Error "Unsupported Operating System" # please implement me
	exit
}

$global:myEnv | ConvertTo-Json | Out-File $myEnvFile -Force

Initialize-SystemEnv

Get-NativeTriplet | Initialize-Vcpkg

Initialize-VcpkgEnv
