. $PSScriptRoot/homebrew.ps1
. $PSScriptRoot/../../platform.ps1

Write-Host "Installing MacOS dependencies..."

$(xcode-select --install) 2>&1

# what the build runs from Homebrew: cmake itself, and pkg-config, which vcpkg finds there for its meson ports (mesa: the
# Vulkan driver, KosmicKrisp, is a vcpkg port) and its .pc fixups
Install-HomebrewPackage pkg-config
Install-HomebrewPackage cmake
# where this machine writes to the shared binary cache (see scripts/binarycache.ps1), vcpkg's x-aws source runs the AWS CLI
if ($env:SPEEDO_BINARY_CACHE_WRITE -and -not (Get-Command "aws" -ErrorAction SilentlyContinue))
{
	Install-HomebrewPackage awscli
}

# resolve the MacOSX.sdk symlink so SDKROOT names the SDK version (e.g. MacOSX27.0.sdk) and changes when the SDK is updated
$sdkPath = xcrun --sdk macosx --show-sdk-path
if ($LASTEXITCODE -ne 0 -or -not $sdkPath)
{
	throw "Could not find the macOS SDK (xcrun exit code $LASTEXITCODE). Are the Xcode Command Line Tools installed? Try 'xcode-select --install'."
}
$global:Env['SDKROOT'] = $(realpath $sdkPath)
# and the oldest macOS what is built runs on: the SDK's major version's first (27.0 for the 27.2 SDK). Without it clang
# takes the SDK's own version (27.2), so packages built with one SDK wouldn't load on an older minor macOS. The osx triplets
# track it in their packages' ABI in place of the SDK's path (the same SDK from Xcode and from the Command Line Tools has
# two paths, so the shared binary cache's packages would match neither), and every compile reads it (clang, CMake and
# meson take MACOSX_DEPLOYMENT_TARGET from the environment)
$sdkVersion = $(xcrun --sdk macosx --show-sdk-version)
$global:Env['MACOSX_DEPLOYMENT_TARGET'] = "$(($sdkVersion -split '\.')[0]).0"
$global:Env.Remove('MACOS_SDK_VERSION')
$global:Env['MACOS_BUILD_VERSION'] = $(sw_vers -buildVersion)
$global:Env['CMAKE_APPLE_SILICON_PROCESSOR'] = $(Get-HostArchitecture)