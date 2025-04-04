. $PSScriptRoot/scripts/env.ps1
. $PSScriptRoot/scripts/platform.ps1
. $PSScriptRoot/scripts/vcpkg.ps1

$global:myEnv = New-Object -TypeName PSObject

$myEnvFile = "$PSScriptRoot/.env.json"

if (Test-Path $myEnvFile)
{
	$global:myEnv = Get-Content -Path $myEnvFile -Raw | ConvertFrom-Json
}

if ($IsWindows)
{
	& $PSScriptRoot/scripts/platforms/windows/windows.ps1
}
elseif ($IsMacOS)
{
	& $PSScriptRoot/scripts/platforms/osx/osx.ps1
}
elseif ($IsLinux)
{
	& $PSScriptRoot/scripts/platforms/linux/linux.ps1
}
else
{
	Write-Error "Unsupported Operating System" # please implement me
	exit
}

$VcpkgRoot = Initialize-Vcpkg
$Arch = Get-NativeArchitecture
$OS = Get-NativeOS

if ($IsWindows)
{
	$SystemTriplet = "$Arch-$OS-clangcl-release"
}
else
{
	$SystemTriplet = "$Arch-$OS-release"
}

$TargetTriplet = "$Arch-$OS-clang"

Write-Host "Installing toolchain for $SystemTriplet using manifest..."

#todo: add --x-abi-tools-use-exact-version?
#$env:VCPKG_FORCE_SYSTEM_BINARIES = "1"
Invoke-Expression("$VcpkgRoot/vcpkg install --x-install-root=$PSScriptRoot/build/toolchain --overlay-triplets=$PSScriptRoot/scripts/cmake/triplets --triplet $SystemTriplet --x-feature=toolchain --no-print-usage")

$toolchainPath = "$PSScriptRoot/build/toolchain/$SystemTriplet"
$llvmToolsPath = "$toolchainPath/tools/llvm"

$global:myEnv | Add-Member -Force -PassThru -NotePropertyName LLVM_PATH -NotePropertyValue $toolchainPath | Out-Null
$global:myEnv | Add-Member -Force -PassThru -NotePropertyName LLVM_TOOLS_PATH -NotePropertyValue $llvmToolsPath | Out-Null
$global:myEnv | ConvertTo-Json | Out-File $myEnvFile -Force

Initialize-SystemEnv
Initialize-VcpkgEnv

Write-Host "Installing packages for $TargetTriplet using manifest..."

Invoke-Expression("$VcpkgRoot/vcpkg install --x-install-root=$PSScriptRoot/build/packages --overlay-triplets=$PSScriptRoot/scripts/cmake/triplets --triplet $TargetTriplet --x-feature=client --x-feature=server --no-print-usage")

Initialize-VcpkgToolsEnv
