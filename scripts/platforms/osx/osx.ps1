. $PSScriptRoot/homebrew.ps1
. $PSScriptRoot/../../platform.ps1

Write-Host "Installing MacOS dependencies..."

$(xcode-select --install) 2>&1

Install-HomebrewPackage coreutils
Install-HomebrewPackage pkg-config
Install-HomebrewPackage libxinerama
Install-HomebrewPackage libxxf86vm
Install-HomebrewPackage libxcursor
Install-HomebrewPackage mesa
Install-HomebrewPackage mesa-glu
Install-HomebrewPackage patchelf
Install-HomebrewPackage molten-vk
Install-HomebrewPackage cmake

# resolve the MacOSX.sdk symlink so SDKROOT names the SDK version (e.g. MacOSX27.0.sdk) and changes when the SDK is updated
$sdkPath = xcrun --sdk macosx --show-sdk-path
if ($LASTEXITCODE -ne 0 -or -not $sdkPath)
{
	throw "Could not find the macOS SDK (xcrun exit code $LASTEXITCODE). Are the Xcode Command Line Tools installed? Try 'xcode-select --install'."
}
$global:myEnv['SDKROOT'] = $(realpath $sdkPath)
$global:myEnv['MACOS_BUILD_VERSION'] = $(sw_vers -buildVersion)
$global:myEnv['CMAKE_APPLE_SILICON_PROCESSOR'] = $(Get-HostArchitecture)