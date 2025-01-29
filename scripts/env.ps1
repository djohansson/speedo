. $PSScriptRoot/platform.ps1

function Add-EnvDylibPath
{
	param([Parameter(Mandatory = $True, Position = 0)] [string] $path)

	if (-not (Test-Path $path))
	{
		Write-Error "Path does not exist: $path"

		return
	}

	if ($IsWindows)
	{
		if ("" -eq $env:PATH -or $null -eq $env:PATH)
		{
			$env:PATH = $path
		}
		else
		{
			$env:PATH += [IO.Path]::PathSeparator + $path
		}
	}
	elseif ($IsMacOS)
	{
		if ("" -eq $env:DYLD_LIBRARY_PATH -or $null -eq $env:DYLD_LIBRARY_PATH)
		{
			$env:DYLD_LIBRARY_PATH = $path
		}
		else
		{
			$env:DYLD_LIBRARY_PATH += [IO.Path]::PathSeparator + $path
		}
	}
	elseif ($IsLinux)
	{
		if ("" -eq $env:LD_LIBRARY_PATH -or $null -eq $env:LD_LIBRARY_PATH)
		{
			$env:LD_LIBRARY_PATH = $path
		}
		else
		{
			$env:LD_LIBRARY_PATH += [IO.Path]::PathSeparator + $path
		}
	}
	else
	{
		Write-Error "Unsupported Operating System" # please implement me
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
	$Triplet = Get-TargetTriplet

	#Write-Host "Adding installed vcpkg packages to env:PATH..."
	$BinDirectory = "$PSScriptRoot/../build/packages/$Triplet/bin"
	if (Test-Path $BinDirectory)
	{
		#Write-Host $BinDirectory
		Add-EnvPath $BinDirectory
	}

	#Write-Host "Adding installed vcpkg packages dylib/dll/so:s..."
	$DylibDirectory = "$PSScriptRoot/../build/packages/$Triplet"
	if (Test-Path $DylibDirectory)
	{
		#Write-Host $DylibDirectory
		Add-EnvDylibPath $DylibDirectory
	}

	foreach($ToolsDirectory in Get-ChildItem -Path $PSScriptRoot/../build/packages/$Triplet/tools -Directory -ErrorAction SilentlyContinue)
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
	$env:CONSOLE_DEVICE = if ($IsWindows) { '\\.\CON' } else { '/dev/tty'}
	
	#Write-Host "Setting system (package manager) environment variables..."
	Read-EnvFile "$PSScriptRoot/../.env.json"

	#Write-Host "Adding toolchain dylib/dll/so:s..."
	$DylibDirectory = "$PSScriptRoot/../build/toolchain/$Triplet"
	if (Test-Path $DylibDirectory)
	{
		#Write-Host $DylibDirectory
		Add-EnvDylibPath $DylibDirectory
	}
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
