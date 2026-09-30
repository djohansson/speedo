. $PSScriptRoot/winget.ps1
. $PSScriptRoot/vs.ps1

Write-Host "Installing Windows dependencies..."

Assert-WinGet

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
# editions call it NativeDesktop), so an existing Community/Professional/Enterprise install qualifies
$VSComponents = @("Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "Microsoft.VisualStudio.Component.VC.ATL", "Microsoft.VisualStudio.Component.VC.Llvm.Clang", "Microsoft.VisualStudio.Component.Windows11SDK.22621")
$VSInstance = Get-VSInstance -Require $VSComponents
if (-not ($VSInstance))
{
	Write-Host "Installing VisualStudio 2022 VC BuildTools..."

	$ExitCode = Invoke-WinGetInstall -Id Microsoft.VisualStudio.2022.BuildTools -Override "--quiet --add Microsoft.VisualStudio.Workload.VCTools;includeRecommended --add Microsoft.VisualStudio.Component.VC.ATL --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --wait"

	$VSInstance = Get-VSInstance -Require $VSComponents
	if (-not ($VSInstance)) { Write-Error "Visual Studio 2022 VC BuildTools installation failed (winget exit code $ExitCode)" }
}

# the Windows SDK root is not necessarily under C:\Program Files (x86), read it from the registry
$winSDKRoot = (Get-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots" -Name KitsRoot10 -ErrorAction SilentlyContinue).KitsRoot10
if (-not ($winSDKRoot)) { Write-Error "Windows 10/11 SDK not found (no KitsRoot10 in the registry)" }
$winSDKRoot = $winSDKRoot.TrimEnd('\')

$winSDKManifest = [xml](Get-Content -Path "$winSDKRoot\SDKManifest.xml")
$platformIndentityStr = $winSDKManifest.FileList.PlatformIdentity
$windowsSdkVersion = $platformIndentityStr.SubString($platformIndentityStr.LastIndexOf("Version=") + 8)

$global:myEnv['WINDOWS_SDK'] = $winSDKRoot
$global:myEnv['WINDOWS_SDK_VERSION'] = $windowsSdkVersion
$global:myEnv['VISUAL_STUDIO_PATH'] = $VSInstance.installationPath
$global:myEnv['VISUAL_STUDIO_VCTOOLS_VERSION'] = (Get-Content -Path ($VSInstance.installationPath + "\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt"))
