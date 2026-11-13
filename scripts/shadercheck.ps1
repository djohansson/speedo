<#
.SYNOPSIS
Every shader layout the client loads, compiled by slang to another backend's target: Metal's MSL (checked by Metal's
own compiler with -Validate) or D3D12's HLSL (docs/backends-plan.md).

.DESCRIPTION
The layouts are the files and variants the client compiles (gfx::SceneShaderLayouts with ray queries and without,
ImGuiShaderLayout, EnvironmentFilter's): each file's entry points come from slang's reflection of it (its [shader]
functions), each compiled alone with the client's options (shadercompiler.cpp), as the Metal backend will: slang can't
put all of a file's into one Metal library (it makes groupshared memory module scope threadgroup variables, which a
vertex function can't have). HLSL for shader model 6.6, a register space per descriptor set (each set is a parameter
block, see bindings.slang).

-Validate (macOS, metal) compiles each MSL file with Metal's runtime compiler (src/tools/macos/mslcheck.mm, built into
the work dir with the command line tools' clang). DXIL needs DXC, which slang loads and macOS doesn't have: validate
HLSL on Windows. Until the Metal backend compiles the shaders itself (docs/backends-plan.md, step 4), this is the check
after a shader change: it takes about a minute.

.EXAMPLE
pwsh scripts/shadercheck.ps1 -Preset arm64-osx-clang-profile -Target metal -Validate
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)] [string] $Preset,
	[Parameter(Mandatory = $true)] [ValidateSet('metal', 'hlsl')] [string] $Target,
	[string] $Work,
	[switch] $Validate
)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$triplet = $Preset.Substring(0, $Preset.LastIndexOf('-'))
$exe = if ($IsWindows) { '.exe' } else { '' }
$slangc = Join-Path $root "install/$triplet/tools/shader-slang/slangc$exe"
if (-not (Test-Path -LiteralPath $slangc -PathType Leaf))
{
	[Console]::Error.WriteLine("no ${slangc}: run setup first")
	exit 2
}
$shaders = Join-Path $root 'src/gfx/shaders'
if (-not $Work)
{
	$Work = Join-Path ([IO.Path]::GetTempPath()) "speedo-shadercheck-$PID"
}
$out = Join-Path $Work $Target
New-Item -ItemType Directory -Force -Path $out | Out-Null

# the client's options (see ShaderLoader::SlangConfiguration and the layouts' configurations)
$common = @('-I', $shaders, '-D', 'SHADERTYPES_H_GPU_TARGET=true', '-O3', '-matrix-layout-column-major')
$targetArgs = switch ($Target)
{
	'metal' { @('-target', 'metal') }
	'hlsl' { @('-target', 'hlsl', '-profile', 'sm_6_6', '-parameter-blocks-use-register-spaces') }
}
$layouts = @(
	@{ Name = 'main'; File = 'shaders.slang'; Defines = @() }
	@{ Name = 'main-rq'; File = 'shaders.slang'; Defines = @('-D', 'BVH_RAY_QUERY=1') }
	@{ Name = 'flare'; File = 'flare.slang'; Defines = @() }
	@{ Name = 'environment'; File = 'environment.slang'; Defines = @() }
	@{ Name = 'imgui'; File = 'imgui.slang'; Defines = @() }
)

$failed = [System.Collections.Generic.List[string]]::new()
$files = [System.Collections.Generic.List[string]]::new()
$start = Get-Date
foreach ($layout in $layouts)
{
	$source = Join-Path $shaders $layout.File
	# the entry points, from a compile of the whole file's reflection (a regex, not ConvertFrom-Json: slang 2026.22's
	# json of the main layout has an invalid escape): the name and stage pairs after "entryPoints" at the first one's
	# indentation (their parameters have a stage too, deeper)
	$json = Join-Path $Work "$($layout.Name).json"
	& $slangc $source @common @($layout.Defines) -target spirv -o (Join-Path $Work "$($layout.Name).spv") -reflection-json $json *> (Join-Path $Work "$($layout.Name).log")
	if ($LASTEXITCODE -ne 0)
	{
		$failed.Add("$($layout.Name): the reflection's compile failed (see $(Join-Path $Work "$($layout.Name).log"))")
		continue
	}
	$text = Get-Content -Raw -LiteralPath $json
	$text = $text.Substring([Math]::Max(0, $text.IndexOf('"entryPoints"')))
	$pairs = [regex]::Matches($text, '(?m)^([ \t]*)"name":\s*"(\w+)",\s*\r?\n\1"stage":\s*"(\w+)"')
	$entryPoints = @($pairs | Where-Object { $pairs.Count -gt 0 -and $_.Groups[1].Value -eq $pairs[0].Groups[1].Value })
	foreach ($entryPoint in $entryPoints)
	{
		$name = $entryPoint.Groups[2].Value
		$stage = $entryPoint.Groups[3].Value
		$file = Join-Path $out "$($layout.Name)-$name.$Target"
		$log = Join-Path $out "$($layout.Name)-$name.log"
		& $slangc $source @common @($layout.Defines) @targetArgs -entry $name -stage $stage -o $file *> $log
		if ($LASTEXITCODE -ne 0)
		{
			$first = Select-String -LiteralPath $log -Pattern 'error' | Select-Object -First 1
			$failed.Add("$($layout.Name) ${name}: $(if ($first) { $first.Line.Trim() } else { "slangc exited with $LASTEXITCODE" })")
			continue
		}
		$files.Add($file)
	}
}
Write-Host "$Target`: $($files.Count) of $($files.Count + $failed.Count) entry points compiled by slang in $([int]((Get-Date) - $start).TotalSeconds) s"

if ($Validate -and $Target -eq 'metal')
{
	if (-not $IsMacOS)
	{
		[Console]::Error.WriteLine('-Validate needs macOS for the metal target')
		exit 2
	}
	$checker = Join-Path $Work 'mslcheck'
	& xcrun clang++ -std=c++20 -fobjc-arc -framework Metal -framework Foundation (Join-Path $root 'src/tools/macos/mslcheck.mm') -o $checker
	if ($LASTEXITCODE -ne 0)
	{
		[Console]::Error.WriteLine('building mslcheck failed')
		exit 2
	}
	$result = & $checker @files
	foreach ($line in $result)
	{
		if ($line -match '^FAIL ')
		{
			$failed.Add("metal's compiler: $($line.Substring(5))")
		}
		elseif ($line -match '^\s+.*error' -and $failed.Count -lt 400)
		{
			$failed.Add("    $($line.Trim())")
		}
	}
	Write-Host "metal's compiler: $(@($result | Where-Object { $_ -match '^OK ' }).Count) of $($files.Count) passed"
}

foreach ($line in $failed | Select-Object -First 60)
{
	Write-Host "FAIL $line"
}
Write-Host "work dir: $Work"
exit $(if ($failed.Count -eq 0) { 0 } else { 1 })
