. $PSScriptRoot/platform.ps1

function Add-EnvPath
{
	param([Parameter(Mandatory = $True, Position = 0)] [string] $path)

	if (-not (Test-Path $path))
	{
		Write-Error "Path does not exist: $path"

		return
	}

	if ("" -eq $env:PATH -or $null -eq $env:PATH)
	{
		$env:PATH = $path
	}
	else
	{
		$env:PATH += [IO.Path]::PathSeparator + $path
	}
}

function Add-EnvDylibPath
{
	param([Parameter(Mandatory = $True, Position = 0)] [string] $path)

	if (-not (Test-Path $path))
	{
		Write-Error "Path does not exist: $path"

		return
	}

	if ("" -eq $env:DYLD_LIBRARY_PATH -or $null -eq $env:DYLD_LIBRARY_PATH)
	{
		$env:DYLD_LIBRARY_PATH = $path
	}
	else
	{
		$env:DYLD_LIBRARY_PATH += [IO.Path]::PathSeparator + $path
	}
}

function Read-EnvFile
{
	param([Parameter(Mandatory = $True, Position = 0)] [string] $envFile)

	if (-not (Test-Path $envFile))
	{
		Write-Error "$envFile does not exist"

		return
	}
	
	$localEnv = Get-Content -Path $envFile -Raw | ConvertFrom-Json

	foreach($property in $localEnv.psobject.properties)
	{
		[Environment]::SetEnvironmentVariable($property.Name, $property.Value)

		#Write-Host $property.Name "=" $property.Value
	}
}

function Invoke-Sudo
{ 
	& /usr/bin/env sudo pwsh -command "& $args" 
}

function Initialize-VcpkgEnv
{
	$Triplet = Get-NativeTriplet

	#Write-Host "Adding installed vcpkg packages to env:PATH..."
	$BinDirectory = "$PSScriptRoot/../build.vcpkg/$Triplet/bin"
	if (Test-Path $BinDirectory)
	{
		#Write-Host $BinDirectory
		Add-EnvPath $BinDirectory
	}

	#Write-Host "Adding installed vcpkg packages to env:DYLD_LIBRARY_PATH..."
	$LibDirectory = "$PSScriptRoot/../build.vcpkg/$Triplet/lib"
	if (Test-Path $LibDirectory)
	{
		#Write-Host $LibDirectory
		Add-EnvDylibPath $LibDirectory
	}

	foreach($ToolsDirectory in Get-ChildItem -Path $PSScriptRoot/../build.vcpkg/$Triplet/tools -Directory -ErrorAction SilentlyContinue)
	{
		#Write-Host "Adding " $ToolsDirectory " to env:PATH..."
		Add-EnvPath $ToolsDirectory

		if ($IsLinux -or $IsMacOS)
		{
			foreach ($Tool in Get-ChildItem $ToolsDirectory -File -Recurse -ErrorAction SilentlyContinue)
			{
				#Write-Host "Granting executable rights to " $Tool 
				Invoke-Expression "chmod +x $Tool"
				#Invoke-Sudo "chmod +x $Tool"
			}
		}
	}
}

function Initialize-SystemEnv
{
	#Write-Host "Initializing development environment..."
	$env:TARGET_ARCHITECTURE = Get-NativeArchitecture
	$env:TARGET_OS = Get-NativeOS
	$env:TARGET_COMPILER = Get-NativeCompiler
	
	#Write-Host "Setting system (package manager) environment variables..."
	Read-EnvFile "$PSScriptRoot/../.env.json"
}

function Invoke-VcpkgEnv
{
	param(
		[Parameter(Mandatory = $True, Position = 0)] [string] $command,
		[Parameter(Mandatory = $True, ValueFromRemainingArguments = $true, Position = 1)][string[]] $argsList
	)

	Initialize-SystemEnv
	Initialize-VcpkgEnv

	$arguments = Join-String -Separator " " -InputObject $argsList
	$expr = ($command + " " + $arguments)

	#Write-Host $expr
	Invoke-Expression $expr
}
