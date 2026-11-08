<#
.SYNOPSIS
Makes the application's icons from scripts/package/speedo.svg: speedo.icns (the macOS bundle's) and speedo.ico (the Windows
executables', their windows' and the installer's), beside it. macOS only; the outputs are committed.

.DESCRIPTION
Renders the SVG through src/tools/macos/svgrender.swift (AppKit's SVG support, built with swiftc into build/tools), then:

- speedo.icns: the macOS icon set's ten images, 16 to 1024 pixels, by iconutil. The SVG is drawn on macOS's icon grid
  (an 824 pixel tile on a 1024 canvas), which the system expects.
- speedo.ico: 16 to 256 pixels, each a PNG (as Windows reads them since Vista), from the SVG's tile with a thin margin
  rather than macOS's grid, whose margins would shrink the small sizes Windows shows most.

The AppImage takes speedo.svg as it is. Run it after changing the SVG, and commit the three files.

.EXAMPLE
scripts/package/make-icons.ps1
#>
$ErrorActionPreference = 'Stop'

if (-not $IsMacOS)
{
	throw 'make-icons.ps1 renders through AppKit: run it on macOS'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$svg = Join-Path $PSScriptRoot 'speedo.svg'
$tools = Join-Path $root 'build/tools'
$work = Join-Path $tools 'icons'
Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $work | Out-Null

function Invoke-Native
{
	$command, $arguments = $args
	& $command @arguments
	if ($LASTEXITCODE -ne 0)
	{
		throw "$command $($arguments -join ' ') failed ($LASTEXITCODE)"
	}
}

$renderer = Join-Path $tools 'svgrender'
Invoke-Native swiftc -O (Join-Path $root 'src/tools/macos/svgrender.swift') -o $renderer

# macOS: the icon set's sizes, each at 1x and 2x
$iconset = Join-Path $work 'speedo.iconset'
New-Item -ItemType Directory -Path $iconset | Out-Null
foreach ($points in 16, 32, 128, 256, 512)
{
	Invoke-Native $renderer $svg $points (Join-Path $iconset "icon_${points}x${points}.png")
	Invoke-Native $renderer $svg ($points * 2) (Join-Path $iconset "icon_${points}x${points}@2x.png")
}
Invoke-Native iconutil -c icns $iconset -o (Join-Path $PSScriptRoot 'speedo.icns')

# Windows: the tile with a thin margin (the SVG's viewBox narrowed to it)
$tileSvg = Join-Path $work 'speedo-tile.svg'
$text = [System.IO.File]::ReadAllText($svg)
$narrowed = $text -replace 'viewBox="0 0 1024 1024"', 'viewBox="80 80 864 864"'
if ($narrowed -eq $text)
{
	throw "speedo.svg's viewBox isn't 0 0 1024 1024: update make-icons.ps1's tile"
}
[System.IO.File]::WriteAllText($tileSvg, $narrowed)

$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256
$images = foreach ($size in $sizes)
{
	$png = Join-Path $work "tile-$size.png"
	Invoke-Native $renderer $tileSvg $size $png
	, [System.IO.File]::ReadAllBytes($png)
}

# an .ico: its header, a 16 byte entry an image (a 256 pixel one's width and height written 0), then the images
$stream = [System.IO.MemoryStream]::new()
$writer = [System.IO.BinaryWriter]::new($stream)
$writer.Write([uint16]0) # reserved
$writer.Write([uint16]1) # an icon
$writer.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($index = 0; $index -lt $sizes.Count; $index++)
{
	$size = $sizes[$index]
	$writer.Write([byte]($size -ge 256 ? 0 : $size))
	$writer.Write([byte]($size -ge 256 ? 0 : $size))
	$writer.Write([byte]0) # no palette
	$writer.Write([byte]0) # reserved
	$writer.Write([uint16]1) # planes
	$writer.Write([uint16]32) # bits a pixel
	$writer.Write([uint32]$images[$index].Length)
	$writer.Write([uint32]$offset)
	$offset += $images[$index].Length
}
foreach ($image in $images)
{
	$writer.Write($image)
}
$writer.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $PSScriptRoot 'speedo.ico'), $stream.ToArray())

Write-Host "made speedo.icns and speedo.ico from speedo.svg ($($sizes.Count) Windows sizes)"
