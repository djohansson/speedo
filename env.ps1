function Get-Triplet
{
	$Arch = $Env:PROCESSOR_ARCHITECTURE
	$Triplet = ""

	if ($Arch -eq "AMD64")
	{
		$Triplet += "x64-"
	}
	# TODO: Add support for other architectures
	# elseif ($Arch -eq "x86")
	# {
	# 	$Triplet += "x86-"
	# }
	# elseif ($Arch -eq "ARM64")
	# {
	# 	$Triplet += "arm64-"
	# }
	else
	{
		Write-Error "Unsupported architecture: $Arch"
	}

	if ($IsWindows)
	{
		$Triplet += "windows-clang"
	}
	# TODO: Add support for other operating systems
	# elseif ($IsMacOS)
	# {
	# 	$Triplet += "osx-clang"
	# }
	# elseif ($IsLinux)
	# {
	# 	$Triplet += "linux-clang"
	# }
	else
	{
		Write-Error "Unsupported Operating System"
	}

	return $Triplet
}

function Read-EnvFile
{
	param([Parameter(Mandatory = $True, Position = 0)] [string] $envFile)

	$localEnv = Get-Content -Path $envFile -Raw | ConvertFrom-Json

	foreach( $property in $localEnv.psobject.properties)
	{
		[Environment]::SetEnvironmentVariable($property.Name, $property.Value)

		#Write-Host $property.Name "=" $property.Value
	}
}

function Initialize-DevEnv
{
	#Write-Host "Adding installed vcpkg packages to env:Path..."

	$BinDirectory = "$PSScriptRoot\build.vcpkg\$triplet\bin"
	$env:Path += ";$BinDirectory"

	#Write-Host $BinDirectory

	$triplet = Get-Triplet

	foreach($ToolsDirectory in Get-ChildItem -Path $PSScriptRoot\build.vcpkg\$triplet\tools -Directory)
	{
		$env:Path += ";$ToolsDirectory"

		#Write-Host $ToolsDirectory
	}
	
	#Write-Host "Setting environment variables..."

	Read-EnvFile "$PSScriptRoot/env.json"
}

function Invoke-DevEnv
{
	param(
		[Parameter(Mandatory = $True, Position = 0)] [string] $command,
		[Parameter(Mandatory = $True, ValueFromRemainingArguments = $true, Position = 1)][string[]] $argsList
	)

	Initialize-DevEnv

	$arguments = Join-String -Separator " " -InputObject $argsList

	Invoke-Expression ($command + " " + $arguments)
}
