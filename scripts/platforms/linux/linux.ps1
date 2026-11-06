. $PSScriptRoot/apt.ps1

Write-Host "Installing Linux dependencies..."

Install-AptPackage autoconf
Install-AptPackage libtool
Install-AptPackage coreutils
Install-AptPackage pkg-config
Install-AptPackage snapd
Install-AptPackage patchelf
Install-AptPackage lsb-release
Install-AptPackage software-properties-common
Install-AptPackage gnupg
Install-AptPackage libxinerama-dev
Install-AptPackage libxcursor-dev
Install-AptPackage libglu1-mesa-dev
Install-AptPackage libwayland-dev
Install-AptPackage xorg-dev
Install-AptPackage libxkbcommon-dev
Install-AptPackage cmake

# the C library's version, which the linux clang triplets track in their packages' ABI: a package built against a newer
# glibc fails to load with an older one, so the shared binary cache's packages must match it (e.g. "glibc 2.39")
$global:Env['GLIBC_VERSION'] = $(getconf GNU_LIBC_VERSION 2>$null) ?? 'unknown'

#$env:VCPKG_FORCE_SYSTEM_BINARIES = "1"
#todo: add --x-abi-tools-use-exact-version?
