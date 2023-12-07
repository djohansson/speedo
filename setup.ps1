. $PSScriptRoot/platform.ps1
. $PSScriptRoot/vcpkg.ps1

$global:myEnv = New-Object -TypeName PSObject

$myEnvFile = "$PSScriptRoot/env.json"

if (Test-Path $myEnvFile)
{
	$global:myEnv = Get-Content -Path $myEnvFile -Raw | ConvertFrom-Json
}

if ($IsWindows)
{
	& $PSScriptRoot/setup_windows.ps1
}
elseif ($IsMacOS)
{
	& $PSScriptRoot/setup_macos.ps1
}
else
{
	Write-Error "Unsupported Operating System"
}

$global:myEnv | ConvertTo-Json | Out-File $myEnvFile

Initialize-Vcpkg $(Get-NativeTriplet)
