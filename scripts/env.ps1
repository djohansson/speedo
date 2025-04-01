. $PSScriptRoot/platform.ps1

function Add-EnvPath
{
	param(
		[Parameter(Mandatory = $True, Position = 0)] [string] $path,
		[Parameter(Mandatory = $False, Position = 1)] [EnvironmentVariableTarget] $scope = [EnvironmentVariableTarget]::Process
	)

	if (-not (Test-Path $path))
	{
		Write-Error "Path does not exist: $path"

		return
	}

	if ("" -eq $env:PATH -or $null -eq $env:PATH)
	{
		[Environment]::SetEnvironmentVariable("PATH", $path, $scope)
	}
	else
	{
		[Environment]::SetEnvironmentVariable("PATH", $env:PATH + [IO.Path]::PathSeparator + $path, $scope)
	}
}

function Add-EnvDylibPath
{
	param(
		[Parameter(Mandatory = $True, Position = 0)] [string] $path,
		[Parameter(Mandatory = $False, Position = 1)] [EnvironmentVariableTarget] $scope = [EnvironmentVariableTarget]::Process
	)

	if (-not (Test-Path $path))
	{
		Write-Warning "Path does not exist: $path"
	}

	if ($IsWindows)
	{
		Add-EnvPath $path $scope
	}
	elseif ($IsMacOS)
	{
		if ("" -eq $env:DYLD_LIBRARY_PATH -or $null -eq $env:DYLD_LIBRARY_PATH)
		{
			[Environment]::SetEnvironmentVariable("DYLD_LIBRARY_PATH", $path, $scope)
		}
		else
		{
			[Environment]::SetEnvironmentVariable("DYLD_LIBRARY_PATH", $env:DYLD_LIBRARY_PATH + [IO.Path]::PathSeparator + $path, $scope)
		}
	}
	elseif ($IsLinux)
	{
		if ("" -eq $env:LD_LIBRARY_PATH -or $null -eq $env:LD_LIBRARY_PATH)
		{
			[Environment]::SetEnvironmentVariable("LD_LIBRARY_PATH", $path, $scope)
		}
		else
		{
			[Environment]::SetEnvironmentVariable("LD_LIBRARY_PATH", $env:LD_LIBRARY_PATH + [IO.Path]::PathSeparator + $path, $scope)
		}
	}
	else
	{
		Write-Error "Unsupported Operating System" # please implement me
	}
}

function Read-EnvFile
{
	param(
		[Parameter(Mandatory = $True, Position = 0)] [string] $envFile,
		[Parameter(Mandatory = $False, Position = 1)] [EnvironmentVariableTarget] $scope = [EnvironmentVariableTarget]::Process
	)

	if (-not (Test-Path $envFile))
	{
		Write-Error "$envFile does not exist"

		return
	}
	
	$localEnv = Get-Content -Path $envFile -Raw | ConvertFrom-Json

	foreach($property in $localEnv.psobject.properties)
	{
		[Environment]::SetEnvironmentVariable($property.Name, $property.Value, $scope)

		#Write-Host $property.Name "=" $property.Value
	}
}

function Invoke-Sudo
{ 
	& /usr/bin/env sudo pwsh -command "& $args" 
}

function Initialize-VcpkgEnv
{
	$packageRoot = "$PSScriptRoot/../build/packages/$env:TARGET_TRIPLET"
	
	if ($IsWindows)
	{
		Add-EnvDylibPath "$packageRoot/bin"
		Add-EnvDylibPath "$packageRoot/debug/bin"
	}
	else
	{
		Add-EnvDylibPath "$packageRoot/lib"
		Add-EnvDylibPath "$packageRoot/debug/lib"
	}
}

function Initialize-SystemEnv
{
	#Write-Host "Initializing development environment..."
	$env:TARGET_ARCHITECTURE = Get-NativeArchitecture
	$env:TARGET_OS = Get-NativeOS
	$env:TARGET_TRIPLET = Get-TargetTriplet
	$env:CONSOLE_DEVICE = if ($IsWindows) { '\\.\CON' } else { '/dev/tty'}

	#Write-Host "Adding toolchain dylib/dll/so:s..."
	$toolchainRoot = "$PSScriptRoot/../build/toolchain/$env:TARGET_ARCHITECTURE-$env:TARGET_OS-release"
	if ($IsWindows)
	{
		$dynlibPath = "$toolchainRoot/bin"
	}
	else
	{
		$dynlibPath = "$toolchainRoot/lib"
	}
	if (Test-Path $dynlibPath)
	{
		#Write-Host "Adding $dynlibPath"
		Add-EnvDylibPath $dynlibPath
	}

	#Write-Host "Setting system (package manager) environment variables..."
	Read-EnvFile "$PSScriptRoot/../.env.json"
}

function Initialize-VcpkgToolsEnv
{ 
	foreach($toolsPath in Get-ChildItem -Path $PSScriptRoot/../build/packages/$env:TARGET_TRIPLET/tools -Directory -ErrorAction SilentlyContinue)
	{
		Add-EnvPath $toolsPath

		if ($IsLinux -or $IsMacOS)
		{
			foreach ($tool in Get-ChildItem $toolsPath -File -Recurse -ErrorAction SilentlyContinue)
			{
				#Write-Host "Granting executable rights to " $tool 
				Invoke-Expression "chmod +x $tool"
				#Invoke-Sudo "chmod +x $tool"
			}
		}
	}
}

function Invoke-VcpkgEnv
{
	param(
		[Parameter(Mandatory = $True, Position = 0)] [string] $command,
		[Parameter(Mandatory = $True, ValueFromRemainingArguments = $true, Position = 1)][string[]] $argsList
	)

	Initialize-VcpkgEnv

	$arguments = Join-String -Separator " " -InputObject $argsList
	$expr = ($command + " " + $arguments)

	#Write-Host $expr
	Invoke-Expression $expr
}
