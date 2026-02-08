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

if (!(Test-Path Variable:\IsWindows) -or $IsWindows) 
{
	if (!(Test-Path $PSScriptRoot/vcpkg/vcpkg.exe))
	{
		Invoke-Expression("$PSScriptRoot/vcpkg/bootstrap-vcpkg.bat")
	}
}
else
{
	if (!(Test-Path $PSScriptRoot/vcpkg/vcpkg))
	{
		Invoke-Expression("sh $PSScriptRoot/vcpkg/bootstrap-vcpkg.sh")
	}
}

Initialize-SystemEnv

# compiler is left out when targeting the host system, as vcpkg will automatically select the correct compiler
# use release configuration for the toolchain, as debug builds of the toolchain are not needed.
$HostTriplet = $(Get-HostArchitecture) + '-' + $(Get-HostOS) + '-' + 'release'

Write-Host "Building toolchain using $HostTriplet..."

Invoke-Expression("$PSScriptRoot/vcpkg/vcpkg install --x-install-root=$PSScriptRoot/build/toolchain --overlay-triplets=$PSScriptRoot/scripts/cmake/triplets --triplet $HostTriplet --x-feature=toolchain --x-abi-tools-use-exact-versions --no-print-usage")
