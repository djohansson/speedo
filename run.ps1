param([string]$config='release')

. $PSScriptRoot/scripts/env.ps1
. $PSScriptRoot/scripts/platform.ps1

$triplet = Get-NativeTriplet

Add-EnvPath $PSScriptRoot/build.vcpkg/$triplet/bin
Add-EnvDylibPath $PSScriptRoot/build.vcpkg/$triplet/lib

& $PSScriptRoot/build.output/$config/client -u '$HOME/.speedo' &
& $PSScriptRoot/build.output/$config/server -u '$HOME/.speedo'
