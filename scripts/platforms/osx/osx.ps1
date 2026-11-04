. $PSScriptRoot/homebrew.ps1
. $PSScriptRoot/../../platform.ps1

Write-Host "Installing MacOS dependencies..."

$(xcode-select --install) 2>&1

# what the build runs from Homebrew: cmake itself, and pkg-config, which vcpkg finds there for its meson ports (mesa: the
# Vulkan driver, KosmicKrisp, is a vcpkg port) and its .pc fixups
Install-HomebrewPackage pkg-config
Install-HomebrewPackage cmake

# resolve the MacOSX.sdk symlink so SDKROOT names the SDK version (e.g. MacOSX27.0.sdk) and changes when the SDK is updated
$sdkPath = xcrun --sdk macosx --show-sdk-path
if ($LASTEXITCODE -ne 0 -or -not $sdkPath)
{
	throw "Could not find the macOS SDK (xcrun exit code $LASTEXITCODE). Are the Xcode Command Line Tools installed? Try 'xcode-select --install'."
}
$global:Env['SDKROOT'] = $(realpath $sdkPath)
$global:Env['MACOS_BUILD_VERSION'] = $(sw_vers -buildVersion)
$global:Env['CMAKE_APPLE_SILICON_PROCESSOR'] = $(Get-HostArchitecture)