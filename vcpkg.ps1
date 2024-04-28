. $PSScriptRoot/env.ps1

function Initialize-Vcpkg
{
	param([Parameter(Mandatory = $True, ValueFromPipeline=$True, Position = 0)] [string] $triplet)

	Write-Host "Setting up vcpkg environment..."

	if (Test-Path -Path "~/.vcpkg")
	{
		. ~/.vcpkg/vcpkg-init.ps1
	}
	else
	{
		Write-Host "Installing vcpkg..."

		Invoke-Expression(Invoke-WebRequest -useb https://aka.ms/vcpkg-init.ps1)
	}

	Write-Host "Installing vcpkg packages for $triplet using manifest..."

	Initialize-SystemEnv

	vcpkg install --x-install-root="build.vcpkg" --overlay-triplets=$PSScriptRoot --triplet $triplet --x-feature=client --x-feature=server --no-print-usage
}
