# -PortBuildTools: only what building vcpkg ports from source takes, which setup installs when the binary cache lacks a
# package (see setup.ps1); without it, what restoring the packages and building the project take
param([switch] $PortBuildTools)

. $PSScriptRoot/apt.ps1

if ($PortBuildTools)
{
	Write-Host "Installing the Linux tools that build vcpkg ports..."
	# the host triplets build with the system's clang, lld and LLVM's archiver (see x64-linux-release.cmake)
	Install-AptPackage clang
	Install-AptPackage lld
	Install-AptPackage llvm
	Install-AptPackage python3-venv # meson's ports (libsystemd) build in a python venv
	Install-AptPackage ninja-build # and meson's build with the ninja on the path
	Install-AptPackage autoconf # autotools' ports (libxcrypt, libmount, gperf)
	Install-AptPackage autoconf-archive
	Install-AptPackage automake
	Install-AptPackage libtool
	Install-AptPackage patchelf
	return
}

Write-Host "Installing Linux dependencies..."

# what vcpkg's downloads and archives, and the project's build with LLVM's clang (the C library's headers and startup
# files, GCC's runtime) take (a fresh distribution, e.g. WSL's Ubuntu, has none of them; GitHub's images have most). Not
# the tools that build ports from source (-PortBuildTools), which setup installs only when the binary cache lacks a package
Install-AptPackage build-essential
Install-AptPackage curl
Install-AptPackage zip
Install-AptPackage unzip
Install-AptPackage tar
Install-AptPackage coreutils
Install-AptPackage pkg-config
Install-AptPackage snapd
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

# writing to the shared binary cache (scripts/binarycache.ps1) takes the AWS CLI, which vcpkg's x-aws source runs: AWS's
# own installer, since Ubuntu's archive dropped its awscli package in 24.04
if ($env:SPEEDO_BINARY_CACHE_WRITE -and -not (Get-Command "aws" -ErrorAction SilentlyContinue))
{
	Write-Host "Installing the AWS CLI..."

	$AwsArchitecture = (uname -m) -match 'aarch64|arm64' ? 'aarch64' : 'x86_64'
	$AwsTemp = New-Item -ItemType Directory -Path (Join-Path ([IO.Path]::GetTempPath()) "awscli-$PID")
	curl -fsSL -o "$AwsTemp/awscli.zip" "https://awscli.amazonaws.com/awscli-exe-linux-$AwsArchitecture.zip"
	if ($LASTEXITCODE -ne 0) { throw "AWS CLI download failed with exit code $LASTEXITCODE" }
	unzip -q "$AwsTemp/awscli.zip" -d $AwsTemp
	sudo "$AwsTemp/aws/install" --update
	if ($LASTEXITCODE -ne 0) { throw "AWS CLI installation failed with exit code $LASTEXITCODE" }
	Remove-Item -Recurse -Force $AwsTemp
}

# the C library's version, which the linux clang triplets track in their packages' ABI: a package built against a newer
# glibc fails to load with an older one, so the shared binary cache's packages must match it (e.g. "glibc 2.39")
$global:Env['GLIBC_VERSION'] = $(getconf GNU_LIBC_VERSION 2>$null) ?? 'unknown'

#$env:VCPKG_FORCE_SYSTEM_BINARIES = "1"
#todo: add --x-abi-tools-use-exact-version?
