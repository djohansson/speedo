$Arch = $Env:PROCESSOR_ARCHITECTURE
$Triplet = ""

if ($Arch -eq "AMD64")
{
	$Triplet += "x64-"
}
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
	
	return
}

if ($IsWindows)
{
	$Triplet += "windows"
}
# elseif ($IsMacOS)
# {
# 	$Triplet += "osx"
# }
# elseif ($IsLinux)
# {
# 	$Triplet += "linux"
# }
else
{
	Write-Error "Unsupported Operating System"
	
	return
}

$Triplet += "-clang"

if ($IsWindows)
{
	if (-not (Get-InstalledModule VSSetup -ErrorAction 'SilentlyContinue'))
	{
		Write-Host "Installing VSSetup powershell module..."
	
		Install-Module VSSetup -Scope CurrentUser -Confirm:$False -Force
	}

	if (-not (Get-VSSetupInstance | Select-VSSetupInstance -Product * -Latest -Require "Microsoft.VisualStudio.Workload.VCTools"))
	{
		Write-Host "Installing Visual Studio build tools..."

		$TempDir = "$HOME/temp"
		
		if (-not (Test-Path -Path $TempDir))
		{
			New-Item -Path $TempDir -ItemType Directory | Out-Null
		}

		$VSBTExe = "$TempDir/vs_BuildTools.exe"
		
		Invoke-WebRequest -Uri https://aka.ms/vs/17/release/vs_BuildTools.exe -UseBasicParsing -OutFile $VSBTExe
		Start-Process -FilePath $VSBTExe -ArgumentList @("--layout", "$TempDir/vs_buildtools2022_layout", "--arch", "*", "--lang", "en-US", "--add", "Microsoft.VisualStudio.Workload.VCTools;includeRecommended", "--quiet") -Wait
		Start-Process -FilePath $TempDir/vs_buildtools2022_layout/vs_setup.exe -ArgumentList @("--installPath", "$HOME/vs_buildtools2022", "--nocache", "--wait", "--noUpdateInstaller", "--noWeb", "--norestart", "--quiet") -Wait
		
		Remove-Item $VSBTExe
		Remove-Item $TempDir/vs_buildtools2022_layout -Recurse
	}
}

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

Write-Host "Installing vcpkg packages for $Triplet using manifest..."

vcpkg install --overlay-triplets=$PSScriptRoot --triplet $Triplet --x-feature=client --x-feature=server --x-feature=tests --no-print-usage

Write-Host "Adding installed vcpkg packages to path..."

$BinDirectory = "$PSScriptRoot\vcpkg_installed\$Triplet\bin"
$env:Path += ";$BinDirectory"

Write-Host $BinDirectory

foreach($ToolsDirectory in Get-ChildItem -Path $PSScriptRoot\vcpkg_installed\$Triplet\tools -Directory)
{
	$env:Path += ";$ToolsDirectory"

	Write-Host $ToolsDirectory
}
