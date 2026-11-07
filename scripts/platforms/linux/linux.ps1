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

# writing to the shared binary cache (scripts/binarycache.ps1) takes the AWS CLI, which vcpkg's x-aws source runs: AWS's
# own installer, since Ubuntu's archive dropped its awscli package in 24.04
if ($env:SPEEDO_BINARY_CACHE_WRITE -and -not (Get-Command "aws" -ErrorAction SilentlyContinue))
{
	Write-Host "Installing the AWS CLI..."
	Install-AptPackage curl
	Install-AptPackage unzip

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
