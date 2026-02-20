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

	if (!(Test-Path $PSScriptRoot/vcpkg/vcpkg.exe))
	{
		Invoke-Expression("$PSScriptRoot/vcpkg/bootstrap-vcpkg.bat")
	}
}
elseif ($IsMacOS)
{
	& $PSScriptRoot/scripts/platforms/osx/osx.ps1

	if (!(Test-Path $PSScriptRoot/vcpkg/vcpkg))
	{
		Invoke-Expression("sh $PSScriptRoot/vcpkg/bootstrap-vcpkg.sh")
	}
}
elseif ($IsLinux)
{
	& $PSScriptRoot/scripts/platforms/linux/linux.ps1

	if (!(Test-Path $PSScriptRoot/vcpkg/vcpkg))
	{
		Invoke-Expression("sh $PSScriptRoot/vcpkg/bootstrap-vcpkg.sh")
	}
}
else
{
	Write-Error "Unsupported Operating System" # please implement me
	exit
}

# Compiler is left out when targeting the host system, as vcpkg will automatically select the correct compiler.
# Use release configuration for the toolchain, as debug builds of the toolchain are slow to build and not needed.
$env:VCPKG_ROOT = $("$PSScriptRoot" + [IO.Path]::DirectorySeparatorChar + 'vcpkg')
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName "VCPKG_ROOT" -NotePropertyValue $env:VCPKG_ROOT | Out-Null
$env:VCPKG_HOST_TRIPLET = $(Get-HostTriplet)
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName "VCPKG_HOST_TRIPLET" -NotePropertyValue $env:VCPKG_HOST_TRIPLET | Out-Null
$global:myEnv | ConvertTo-Json | Out-File $myEnvFile -Force

Initialize-HostUserEnv

Invoke-Expression("$PSScriptRoot/vcpkg/vcpkg install --vcpkg-root $env:VCPKG_ROOT --x-install-root=$PSScriptRoot/build/toolchain --overlay-triplets=$PSScriptRoot/scripts/cmake/triplets --triplet $Env:VCPKG_HOST_TRIPLET --x-feature=toolchain --x-abi-tools-use-exact-versions --no-print-usage")
