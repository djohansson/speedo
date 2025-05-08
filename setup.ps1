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

$VcpkgRoot = Initialize-Vcpkg
$Arch = Get-NativeArchitecture
$OS = Get-NativeOS

if ($IsWindows)
{
	$SystemTriplet = "$Arch-$OS-clangcl-release"
}
else
{
	$SystemTriplet = "$Arch-$OS-release"
}

Write-Host "Installing toolchain for $SystemTriplet using manifest..."

Invoke-Expression("$VcpkgRoot/vcpkg install --x-install-root=$PSScriptRoot/build/toolchain --overlay-triplets=$PSScriptRoot/scripts/cmake/triplets --triplet $SystemTriplet --x-feature=toolchain --no-print-usage")
