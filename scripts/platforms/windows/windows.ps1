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
