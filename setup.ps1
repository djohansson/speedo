. $PSScriptRoot/scripts/env.ps1
. $PSScriptRoot/scripts/platform.ps1

$global:myEnv = New-Object -TypeName PSObject

$myEnvFile = "$PSScriptRoot/.env.json"

if (Test-Path $myEnvFile)
{
	$global:myEnv = Get-Content -Path $myEnvFile -Raw | ConvertFrom-Json
}

if (!(Test-Path Variable:\IsWindows) -or $IsWindows)
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

$Arch = Get-NativeArchitecture
$OS = Get-NativeOS

if (!(Test-Path Variable:\IsWindows) -or $IsWindows) 
{
	$SystemTriplet = "$Arch-$OS-clangcl-release"
	if (!(Test-Path $PSScriptRoot/vcpkg/vcpkg.exe))
	{
		Invoke-Expression("$PSScriptRoot/vcpkg/bootstrap-vcpkg.bat")
	}
}
else
{
	$SystemTriplet = "$Arch-$OS-release"
	if (!(Test-Path $PSScriptRoot/vcpkg/vcpkg))
	{
		Invoke-Expression("sh $PSScriptRoot/vcpkg/bootstrap-vcpkg.sh")
	}
}

Write-Host "Installing toolchain for $SystemTriplet using manifest..."

Invoke-Expression("$PSScriptRoot/vcpkg/vcpkg install --x-install-root=$PSScriptRoot/build/toolchain --overlay-triplets=$PSScriptRoot/scripts/cmake/triplets --triplet $SystemTriplet --x-feature=toolchain --x-abi-tools-use-exact-versions --no-print-usage")
