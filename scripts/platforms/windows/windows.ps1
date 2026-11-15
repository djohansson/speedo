# -PortBuildTools: what building vcpkg ports from source takes beyond the rest, which setup installs when the binary
# cache lacks a package (see setup.ps1): nothing on Windows, where the project's build needs Visual Studio's C++ tools
# and the Windows SDK anyway (LLVM's clang links against the MSVC runtime and the SDK's libraries), which build ports too
param([switch] $PortBuildTools)

if ($PortBuildTools)
{
	return
}

. $PSScriptRoot/winget.ps1
. $PSScriptRoot/vs.ps1

Write-Host "Installing Windows dependencies..."

$cmakeCmd = Get-Command "cmake" -All -ErrorAction SilentlyContinue | Where-Object Version -GE ([System.Version]"4.2.3.0")
if (-not ($cmakeCmd))
{
	Write-Host "Installing CMake..."

	$ExitCode = Invoke-WinGetInstall -Id Kitware.CMake

	$env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User")

	$cmakeCmd = Get-Command "cmake" -All -ErrorAction SilentlyContinue | Where-Object Version -GE ([System.Version]"4.2.3.0")
	if (-not ($cmakeCmd)) { Write-Error "CMake 4.2.3+ installation failed (winget exit code $ExitCode)" }
}

# require components rather than the VCTools workload, which only exists in Build Tools (the IDE
# editions call it NativeDesktop), so an existing Community/Professional/Enterprise install qualifies; and a Windows 11
# SDK, 22621 or 26100 (GitHub's hosted Windows images have 26100 alone)
$VSComponents = @("Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "Microsoft.VisualStudio.Component.VC.ATL", "Microsoft.VisualStudio.Component.VC.Llvm.Clang")
$VSSdkComponents = @("Microsoft.VisualStudio.Component.Windows11SDK.22621", "Microsoft.VisualStudio.Component.Windows11SDK.26100")
$VSInstance = Get-VSInstance -Require $VSComponents -RequireAny $VSSdkComponents
if (-not ($VSInstance))
{
	Write-Host "Installing VisualStudio 2022 VC BuildTools..."

	$ExitCode = Invoke-WinGetInstall -Id Microsoft.VisualStudio.2022.BuildTools -Override "--quiet --add Microsoft.VisualStudio.Workload.VCTools;includeRecommended --add Microsoft.VisualStudio.Component.VC.ATL --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --wait"

	$VSInstance = Get-VSInstance -Require $VSComponents -RequireAny $VSSdkComponents
	if (-not ($VSInstance)) { Write-Error "Visual Studio 2022 VC BuildTools installation failed (winget exit code $ExitCode)" }
}

# writing to the shared binary cache (scripts/binarycache.ps1) takes the AWS CLI, which vcpkg's x-aws source runs
if ($env:SPEEDO_BINARY_CACHE_WRITE -and -not (Get-Command "aws" -ErrorAction SilentlyContinue))
{
	Write-Host "Installing the AWS CLI..."

	$ExitCode = Invoke-WinGetInstall -Id Amazon.AWSCLI

	$env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User")

	if (-not (Get-Command "aws" -ErrorAction SilentlyContinue)) { Write-Error "AWS CLI installation failed (winget exit code $ExitCode)" }
}

# D3D12's debug layer (d3d12SDKLayers.dll), what validates the D3D12 backend (SPEEDO_D3D12_VALIDATION=1, assettest's
# -Validate on D3D12, a debug build: docs/backends-plan.md): Windows' optional Graphics Tools, not installed by default.
# Opt in (SPEEDO_D3D12_DEBUG_LAYER=1, the user's call: Windows' download of it takes long); without it setup says how.
# Adding a capability takes an administrator: setup asks once (UAC) where it isn't one. Without it the backend runs
# unvalidated, so a declined prompt or a failed install warns and goes on
$DebugLayer = Join-Path ([System.Environment]::SystemDirectory) 'd3d12SDKLayers.dll'
if ($env:SPEEDO_D3D12_DEBUG_LAYER -ne '1' -and -not (Test-Path $DebugLayer))
{
	Write-Host "D3D12's debug layer (Windows' Graphics Tools) isn't installed: SPEEDO_D3D12_DEBUG_LAYER=1 has setup install it (slow), for validating the D3D12 backend"
}
elseif (-not (Test-Path $DebugLayer))
{
	Write-Host "Installing D3D12's debug layer (Windows' Graphics Tools)..."

	# DISM's command line rather than Add-WindowsCapability, which failed (exit 1, its error lost with the elevated window)
	# in the Microsoft Store's PowerShell 7; DISM, elevated the same way, installed it (2026-11-15). 3010: done, a restart
	# pending
	$DismLog = Join-Path ([IO.Path]::GetTempPath()) 'speedo-graphics-tools.log'
	$DismArgs = @('/Online', '/Add-Capability', '/CapabilityName:Tools.Graphics.DirectX~~~~0.0.1.0', '/NoRestart', '/Quiet', "/LogPath:$DismLog")
	$Administrator = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
		[Security.Principal.WindowsBuiltInRole]::Administrator)
	try
	{
		$StartArgs = @{ FilePath = (Join-Path ([System.Environment]::SystemDirectory) 'dism.exe'); ArgumentList = $DismArgs; Wait = $true; PassThru = $true }
		if (-not $Administrator) { $StartArgs['Verb'] = 'RunAs' }
		$Process = Start-Process @StartArgs
		if ($Process.ExitCode -ne 0 -and $Process.ExitCode -ne 3010) { throw "DISM exited with $($Process.ExitCode), see $DismLog" }
	}
	catch
	{
		Write-Warning "D3D12's debug layer wasn't installed ($($_.Exception.Message)): the D3D12 backend runs unvalidated. Install it later from Settings > System > Optional features > Graphics Tools, or run setup again"
	}
	if (Test-Path $DebugLayer) { Write-Host "D3D12's debug layer is installed" }
}

# the Windows SDK root is not necessarily under C:\Program Files (x86), read it from the registry
$winSDKRoot = (Get-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots" -Name KitsRoot10 -ErrorAction SilentlyContinue).KitsRoot10
if (-not ($winSDKRoot)) { Write-Error "Windows 10/11 SDK not found (no KitsRoot10 in the registry)" }
$winSDKRoot = $winSDKRoot.TrimEnd('\')

$winSDKManifest = [xml](Get-Content -Path "$winSDKRoot\SDKManifest.xml")
$platformIndentityStr = $winSDKManifest.FileList.PlatformIdentity
$windowsSdkVersion = $platformIndentityStr.SubString($platformIndentityStr.LastIndexOf("Version=") + 8)

$global:Env['WINDOWS_SDK'] = $winSDKRoot
$global:Env['WINDOWS_SDK_VERSION'] = $windowsSdkVersion
$global:Env['VISUAL_STUDIO_PATH'] = $VSInstance.installationPath
$global:Env['VISUAL_STUDIO_VCTOOLS_VERSION'] = (Get-Content -Path ($VSInstance.installationPath + "\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt"))
