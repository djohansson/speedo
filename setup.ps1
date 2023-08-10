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
	
	return
}

if ($IsWindows)
{
	$Triplet += "windows-clang"
}
# TODO: Add support for other operating systems
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

# TODO: use targeted versions of all these winget packages.
if ($IsWindows)
{
	$myEnvFile = "./env.json"
	$myEnv = New-Object -TypeName PSObject

	if (Test-Path $myEnvFile)
	{
		$myEnv = Get-Content -Path $myEnvFile -Raw | ConvertFrom-Json
	}

	$pwshCmd = Get-Command "pwsh" -All -ErrorAction SilentlyContinue | Where-Object Version -GE ([System.Version]"7.0.0.0")

	if (-not ($pwshCmd))
	{ 
		Write-Host "Installing Powershell Core..."

		winget install -e -h --id Microsoft.PowerShell

		$pwshCmd = Get-Command "pwsh" -All -ErrorAction SilentlyContinue | Where-Object Version -GE ([System.Version]"7.0.0.0")
	}
	$myEnv | Add-Member -Force -PassThru -NotePropertyName POWERSHELL_PATH -NotePropertyValue (Split-Path -Path $pwshCmd.Source) | Out-Null
	
	$pythonCmd = Get-Command "python" -All -ErrorAction SilentlyContinue | Where-Object Version -GE ([System.Version]"3.11.0.0")

	if (-not ($pythonCmd))
	{ 
		Write-Host "Installing Python 3.11..."

		winget install -e -h --id Python.Python.3.11

		$pythonCmd = Get-Command "python" -All -ErrorAction SilentlyContinue | Where-Object Version -GE ([System.Version]"3.11.0.0")
	}
	$myEnv | Add-Member -Force -PassThru -NotePropertyName PYTHON_PATH -NotePropertyValue (Split-Path -Path $pythonCmd.Source) | Out-Null
	
	$gitCmd = Get-Command "git" -All -ErrorAction SilentlyContinue | Where-Object Version -GE ([System.Version]"2.41.0.0")
	
	if (-not ($gitCmd))
	{
		Write-Host "Installing Git..."

		winget install -e -h --id Git.Git

		$gitCmd = Get-Command "git" -All -ErrorAction SilentlyContinue | Where-Object Version -GE ([System.Version]"2.41.0.0")
	}
	$myEnv | Add-Member -Force -PassThru -NotePropertyName GIT_PATH -NotePropertyValue (Split-Path -Path $gitCmd.Source) | Out-Null
	
	winget list --id KhronosGroup.VulkanSDK -e | Out-Null

	if ($LASTEXITCODE)
	{
		Write-Host "Installing VulkanSDK (requires process elevation)..."

		Start-Process pwsh -Verb runas -ArgumentList "-c winget install -e -h --id KhronosGroup.VulkanSDK"

		# load VULKAN_SDK environment variable into current powershell session
		#$env:VULKAN_SDK = [System.Environment]::GetEnvironmentVariable('VULKAN_SDK','Machine')
	}

	if (-not (Get-InstalledModule VSSetup -ErrorAction SilentlyContinue))
	{
		Write-Host "Installing VSSetup powershell module..."
	
		Install-Module VSSetup -Scope CurrentUser -Confirm:$False -Force
	}

	$VSSetupInstance = Get-VSSetupInstance | Select-VSSetupInstance -Product * -Require "Microsoft.VisualStudio.Workload.VCTools","Microsoft.VisualStudio.Component.VC.Llvm.Clang","Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset"

	if (-not ($VSSetupInstance))
	{
		Write-Host "Installing VisualStudio 2022 VC BuildTools..."

		# --force is required to circumvent the fact that Microsoft.VisualStudio.2022.BuildTools could already be installed without the VCTools workload
		winget install -e -h --id Microsoft.VisualStudio.2022.BuildTools --override "--quiet --add Microsoft.VisualStudio.Workload.VCTools;includeRecommended --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset --wait" --force

		$VSSetupInstance = Get-VSSetupInstance | Select-VSSetupInstance -Product * -Require "Microsoft.VisualStudio.Workload.VCTools","Microsoft.VisualStudio.Component.VC.Llvm.Clang","Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset"
	}
	$myEnv | Add-Member -Force -PassThru -NotePropertyName LLVM_PATH -NotePropertyValue (Split-Path -Path ($VSSetupInstance.InstallationPath + "\VC\Tools\Llvm\bin")) | Out-Null
	
	$myEnv | ConvertTo-Json | Out-File $myEnvFile
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

vcpkg install --x-install-root="build.vcpkg-installed" --overlay-triplets=$PSScriptRoot --triplet $Triplet --x-feature=client --x-feature=server --x-feature=tests --no-print-usage

Write-Host "Adding installed vcpkg packages to path..."

$BinDirectory = "$PSScriptRoot\build.vcpkg-installed\$Triplet\bin"
$env:Path += ";$BinDirectory"

Write-Host $BinDirectory

foreach($ToolsDirectory in Get-ChildItem -Path $PSScriptRoot\build.vcpkg-installed\$Triplet\tools -Directory)
{
	$env:Path += ";$ToolsDirectory"

	Write-Host $ToolsDirectory
}
