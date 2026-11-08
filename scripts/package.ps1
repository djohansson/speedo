<#
.SYNOPSIS
Packages the client in this platform's installer: a .dmg on macOS, an .msi on Windows, an AppImage on Linux.

.DESCRIPTION
Installs a build's Client component (CMakeLists.txt's package: the client, the shared libraries it loads, slang's modules,
the shader sources, the resources and the dependencies' licenses) and wraps it, into -Out (build/package by default):

- macOS: Speedo.app in Speedo-<version>-macos-arm64.dmg, beside a link to /Applications. Ad hoc signed (Apple silicon runs
  only signed code) but not by a developer id, so Gatekeeper refuses it at first: System Settings > Privacy & Security >
  Open Anyway, or xattr -dr com.apple.quarantine /Applications/Speedo.app.
- Windows: Speedo-<version>-windows-x64.msi (CPack's WiX generator, the wix .NET tool), into Program Files with a Start
  menu shortcut. Unsigned, so SmartScreen asks at first (More info > Run anyway).
- Linux: Speedo-<version>-linux-x86_64.AppImage (appimagetool, downloaded into build/tools on first use), run as it is.

Unsigned for now (the user's call, 2026-11-07). The preset defaults to this platform's release preset, which must be built.
Prints the package's path.

.EXAMPLE
scripts/package.ps1
#>
param(
	[string] $Preset,
	[string] $Out
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Preset)
{
	$Preset = if ($IsMacOS) { 'arm64-osx-clang-release' } elseif ($IsWindows) { 'x64-windows-clang-release' } else { 'x64-linux-clang-release' }
}
$build = Join-Path $root "build/$Preset"
if (-not (Test-Path (Join-Path $build 'cmake_install.cmake')))
{
	throw "build/$Preset isn't configured: cmake --preset $Preset, then cmake --build build/$Preset"
}
if (-not $Out)
{
	$Out = Join-Path $root 'build/package'
}

$header = Get-Content -Raw (Join-Path $root 'src/version.h')
$version = (@('Major', 'Minor', 'Patch') | ForEach-Object { if ($header -match "$_ = ([0-9]+)") { $Matches[1] } else { throw "no $_ version in src/version.h" } }) -join '.'

function Write-Message([string] $Message) { [Console]::Error.WriteLine($Message) }

function Invoke-Native
{
	$command, $arguments = $args
	& $command @arguments | ForEach-Object { Write-Message $_ }
	if ($LASTEXITCODE -ne 0)
	{
		throw "$command $($arguments -join ' ') failed ($LASTEXITCODE)"
	}
}

$stage = Join-Path $Out 'stage'
Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# the shaders precompiled, every variant the client may load, into the build's precompiled-shaders, which the Client
# component installs in place of their sources: an installed client compiles nothing, so it ships no slang nor shader
# compiler (see src/tools/shaderprecompile.cpp)
$precompile = Join-Path $build "shaderprecompile$(if ($IsWindows) { '.exe' })"
if (-not (Test-Path $precompile))
{
	throw "$precompile is missing: cmake --preset $Preset && cmake --build build/$Preset"
}
Invoke-Native $precompile (Join-Path $root 'src/gfx/shaders') (Join-Path $build 'precompiled-shaders')

if ($IsMacOS)
{
	$app = Join-Path $stage 'Speedo.app'
	Invoke-Native cmake --install $build --component Client --prefix "$app/Contents"

	# the build's absolute rpaths (LLVM's lib, from the toolchain's link flags) out of the binaries: the bundle's Frameworks
	# are theirs (the executable's rpath, and the libraries' @loader_path)
	$binaries = @(Get-Item "$app/Contents/MacOS/client") + @(Get-ChildItem "$app/Contents/Frameworks" -Filter '*.dylib' -File)
	foreach ($binary in $binaries)
	{
		$lines = @(otool -l $binary.FullName)
		for ($lineIt = 0; $lineIt -lt $lines.Count; $lineIt++)
		{
			if ($lines[$lineIt] -match 'cmd LC_RPATH' -and $lines[$lineIt + 2] -match 'path (\S+) \(offset')
			{
				$rpath = $Matches[1]
				if (-not $rpath.StartsWith('@'))
				{
					Invoke-Native install_name_tool -delete_rpath $rpath $binary.FullName
				}
			}
		}
	}

	# ad hoc: the libraries, then the bundle (install_name_tool invalidated their signatures)
	foreach ($binary in $binaries | Where-Object { $_.Name -ne 'client' })
	{
		Invoke-Native codesign --force --sign - $binary.FullName
	}
	Invoke-Native codesign --force --sign - $app
	Invoke-Native codesign --verify --strict --deep $app

	$dmgRoot = Join-Path $stage 'dmg'
	New-Item -ItemType Directory -Path $dmgRoot | Out-Null
	Invoke-Native ditto $app (Join-Path $dmgRoot 'Speedo.app')
	New-Item -ItemType SymbolicLink -Path (Join-Path $dmgRoot 'Applications') -Target '/Applications' | Out-Null
	$package = Join-Path $Out "Speedo-$version-macos-arm64.dmg"
	Invoke-Native hdiutil create -volname "Speedo $version" -srcfolder $dmgRoot -ov -format UDZO $package
}
elseif ($IsWindows)
{
	# CPack's WiX generator (CMakeLists.txt's CPACK_* settings) installs the component itself, with WiX 5 (6 and later
	# ask for the Open Source Maintenance Fee's EULA) and its UI extension (the installer's dialogs)
	$kWixVersion = '5.0.2'
	if (-not (Get-Command wix -ErrorAction SilentlyContinue))
	{
		throw "the wix .NET tool is needed: dotnet tool install --global wix --version $kWixVersion"
	}
	if (-not (@(wix extension list --global) -match "WixToolset.UI.wixext $([regex]::Escape($kWixVersion))"))
	{
		Invoke-Native wix extension add --global "WixToolset.UI.wixext/$kWixVersion"
	}
	Push-Location $stage
	try
	{
		Invoke-Native cpack -G WIX --config (Join-Path $build 'CPackConfig.cmake') -B $stage
	}
	finally
	{
		Pop-Location
	}
	$msi = Get-ChildItem $stage -Filter '*.msi' -File | Select-Object -First 1
	if (-not $msi)
	{
		throw 'cpack made no .msi'
	}
	$package = Join-Path $Out "Speedo-$version-windows-x64.msi"
	Move-Item -LiteralPath $msi.FullName -Destination $package -Force
}
else
{
	$appDir = Join-Path $stage 'Speedo.AppDir'
	Invoke-Native cmake --install $build --component Client --prefix "$appDir/usr"
	Copy-Item (Join-Path $root 'scripts/package/speedo.desktop') $appDir
	Copy-Item (Join-Path $root 'scripts/package/speedo.svg') $appDir
	Push-Location $appDir
	try
	{
		New-Item -ItemType SymbolicLink -Path 'AppRun' -Target 'usr/bin/client' | Out-Null
	}
	finally
	{
		Pop-Location
	}

	# appimagetool, at a release (an AppImage itself: extracted and run, as a runner has no FUSE)
	$kAppImageToolVersion = '1.9.0'
	$tools = Join-Path $root 'build/tools'
	$appImageTool = Join-Path $tools "appimagetool-$kAppImageToolVersion-x86_64.AppImage"
	if (-not (Test-Path $appImageTool))
	{
		New-Item -ItemType Directory -Force -Path $tools | Out-Null
		Invoke-WebRequest -Uri "https://github.com/AppImage/appimagetool/releases/download/$kAppImageToolVersion/appimagetool-x86_64.AppImage" -OutFile $appImageTool
		Invoke-Native chmod +x $appImageTool
	}
	$package = Join-Path $Out "Speedo-$version-linux-x86_64.AppImage"
	$env:ARCH = 'x86_64'
	$env:APPIMAGE_EXTRACT_AND_RUN = '1'
	Invoke-Native $appImageTool --no-appstream $appDir $package
}

Write-Message "packaged $package ($([math]::Round((Get-Item $package).Length / 1MB, 1)) MiB)"
Write-Output $package
