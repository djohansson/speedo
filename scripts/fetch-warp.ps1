<#
.SYNOPSIS
Downloads WARP, Microsoft's software D3D12 adapter, from its NuGet package (Microsoft.Direct3D.WARP), and prints the path
of its d3d10warp.dll, for SPEEDO_D3D12_WARP.

.DESCRIPTION
The package's WARP is newer than Windows' own (Windows 11's 10.0.26100 here): it compiles the shaders when a pipeline is
made, on every core, where Windows' compiles each at its first dispatch on one (ShadowScene's 30 frames at 640x360, 20 s
against 6.5 minutes), and runs the client's ray queries, which crash Windows' (docs/backends-plan.md, WARP). The client
loads it from where SPEEDO_D3D12_WARP points:

  $env:SPEEDO_D3D12_WARP = scripts/fetch-warp.ps1

The version is pinned, the package checked by its sha256. Its license (the package's LICENSE.TXT) is for testing only:
it is never part of an install or a package (scripts/package.ps1 takes nothing from here). It goes under <dir>/warp/<version>
(the dir defaults to $env:SPEEDO_TEST_ASSETS, or resources/test-assets, which git ignores), kept there, so running it
again only prints the path. The dll is the machine's architecture's (x64 or arm64).

Exits with 1 if the download or its check failed.

.EXAMPLE
$env:SPEEDO_D3D12_WARP = scripts/fetch-warp.ps1
#>
param(
	[string] $Dir
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# 1.0.22 (2026-10-27): 1.0.18 and later need DXC's options in gfx/shadercompiler.h (kDxcOptions) around a bug of their
# compiler's; check the client on a new one (scripts/visualtest.ps1 on it) before moving the pin
$version = '1.0.22'
$sha256 = 'afc058f4d1066edc4231a38f3ab3592b1930f7287d4ac700e6a056e0874bb0be'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Dir)
{
	$Dir = if ($env:SPEEDO_TEST_ASSETS) { $env:SPEEDO_TEST_ASSETS } else { Join-Path $root 'resources/test-assets' }
}
$architecture = if ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64') { 'arm64' } else { 'x64' }
$out = Join-Path $Dir "warp/$version"
$dll = Join-Path $out "$architecture/d3d10warp.dll"
if (Test-Path -LiteralPath $dll -PathType Leaf)
{
	$dll
	exit 0
}

New-Item -ItemType Directory -Force -Path $out | Out-Null
$package = Join-Path $out "microsoft.direct3d.warp.$version.nupkg"
try
{
	Invoke-WebRequest -Uri "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.warp/$version/microsoft.direct3d.warp.$version.nupkg" -OutFile $package
}
catch
{
	[Console]::Error.WriteLine("WARP ${version}: the download failed: $_")
	exit 1
}
$actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $package).Hash.ToLowerInvariant()
if ($actual -ne $sha256)
{
	Move-Item -Force -LiteralPath $package -Destination "$package.unverified"
	[Console]::Error.WriteLine("WARP ${version}: the package's sha256 is $actual, not $sha256 (kept as $package.unverified)")
	exit 1
}

# the dll and its license from the package (a zip)
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($package)
try
{
	foreach ($entry in $zip.Entries)
	{
		$target = switch ($entry.FullName)
		{
			"build/native/bin/$architecture/d3d10warp.dll" { $dll }
			'LICENSE.TXT' { Join-Path $out 'LICENSE.TXT' }
			default { $null }
		}
		if ($target)
		{
			New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
			[System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
		}
	}
}
finally
{
	$zip.Dispose()
}
Remove-Item -LiteralPath $package
if (-not (Test-Path -LiteralPath $dll -PathType Leaf))
{
	[Console]::Error.WriteLine("WARP ${version}: no build/native/bin/$architecture/d3d10warp.dll in the package")
	exit 1
}
[Console]::Error.WriteLine("WARP $version ($architecture) into $out")
$dll
# (an exit code of its own: & on a script sets $LASTEXITCODE only where it exits, which CI's step reads)
exit 0
