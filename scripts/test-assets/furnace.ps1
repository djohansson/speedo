# writes FurnaceInterior.gltf: a closed cube, 4 m a side, its six walls facing inward, each a diffuse surface of albedo
# 0.5 (no metal, roughness 1) emitting the same light. inside it every surface's radiance is Le / (1 - albedo), every
# bounce included: twice its emission (the visual testing plan's step 4, visualtest.ps1's furnace check). run it again
# after changing it: pwsh scripts/test-assets/furnace.ps1 (it writes resources/test-models/gltf/FurnaceInterior.gltf)
param([string] $Out = (Join-Path $PSScriptRoot '../../resources/test-models/gltf/FurnaceInterior.gltf'))

$h = 2.0 # half the side
# each face: its inward normal, and two axes spanning it, wound so that the triangles face inward (counterclockwise seen
# from inside)
$faces = @(
	@{ N = @(-1, 0, 0); C = @($h, 0, 0); U = @(0, 0, 1); V = @(0, 1, 0) },  # +x wall, facing -x
	@{ N = @(1, 0, 0); C = @(-$h, 0, 0); U = @(0, 0, -1); V = @(0, 1, 0) }, # -x wall, facing +x
	@{ N = @(0, -1, 0); C = @(0, $h, 0); U = @(1, 0, 0); V = @(0, 0, 1) },  # ceiling, facing down
	@{ N = @(0, 1, 0); C = @(0, -$h, 0); U = @(1, 0, 0); V = @(0, 0, -1) }, # floor, facing up
	@{ N = @(0, 0, -1); C = @(0, 0, $h); U = @(-1, 0, 0); V = @(0, 1, 0) }, # +z wall, facing -z
	@{ N = @(0, 0, 1); C = @(0, 0, -$h); U = @(1, 0, 0); V = @(0, 1, 0) }   # -z wall, facing +z
)
$positions = [System.Collections.Generic.List[float]]::new()
$normals = [System.Collections.Generic.List[float]]::new()
$indices = [System.Collections.Generic.List[uint16]]::new()
foreach ($face in $faces)
{
	$base = $positions.Count / 3
	foreach ($corner in @(@(-1, -1), @(1, -1), @(1, 1), @(-1, 1)))
	{
		for ($axis = 0; $axis -lt 3; $axis++)
		{
			$positions.Add([float]($face.C[$axis] + $h * ($corner[0] * $face.U[$axis] + $corner[1] * $face.V[$axis])))
			$normals.Add([float]$face.N[$axis])
		}
	}
	# counterclockwise as seen from the normal's side: (u, v) corners in order, and the cross of u and v is the normal
	foreach ($i in 0, 1, 2, 0, 2, 3) { $indices.Add([uint16]($base + $i)) }
}
# the winding: check that cross(p1 - p0, p2 - p0) points along each face's normal
for ($f = 0; $f -lt 6; $f++)
{
	$p = @(0, 1, 2) | ForEach-Object { $k = $indices[$f * 6 + $_] * 3; , @($positions[$k], $positions[$k + 1], $positions[$k + 2]) }
	$a = @(($p[1][0] - $p[0][0]), ($p[1][1] - $p[0][1]), ($p[1][2] - $p[0][2]))
	$b = @(($p[2][0] - $p[0][0]), ($p[2][1] - $p[0][1]), ($p[2][2] - $p[0][2]))
	$cross = @(($a[1] * $b[2] - $a[2] * $b[1]), ($a[2] * $b[0] - $a[0] * $b[2]), ($a[0] * $b[1] - $a[1] * $b[0]))
	$dot = $cross[0] * $faces[$f].N[0] + $cross[1] * $faces[$f].N[1] + $cross[2] * $faces[$f].N[2]
	if ($dot -le 0) { throw "face $f is wound outward" }
}

$stream = [System.IO.MemoryStream]::new()
$writer = [System.IO.BinaryWriter]::new($stream)
foreach ($value in $positions) { $writer.Write($value) }
foreach ($value in $normals) { $writer.Write($value) }
foreach ($value in $indices) { $writer.Write($value) }
$writer.Flush()
$bytes = $stream.ToArray()
$positionBytes = $positions.Count * 4
$normalBytes = $normals.Count * 4
$indexBytes = $indices.Count * 2

$gltf = [ordered]@{
	asset = [ordered]@{ version = '2.0'; generator = 'speedo test assets' }
	extensionsUsed = @('KHR_materials_emissive_strength')
	scene = 0
	scenes = @([ordered]@{ name = 'FurnaceInterior'; nodes = @(0) })
	nodes = @([ordered]@{ name = 'furnace'; mesh = 0 })
	meshes = @([ordered]@{
			name = 'furnace'
			primitives = @([ordered]@{ attributes = [ordered]@{ POSITION = 0; NORMAL = 1 }; indices = 2; material = 0 })
		})
	materials = @([ordered]@{
			name = 'diffuse emitting'
			pbrMetallicRoughness = [ordered]@{ baseColorFactor = @(0.5, 0.5, 0.5, 1.0); metallicFactor = 0.0; roughnessFactor = 1.0 }
			emissiveFactor = @(1.0, 1.0, 1.0)
			extensions = [ordered]@{ KHR_materials_emissive_strength = [ordered]@{ emissiveStrength = 1.0 } }
		})
	buffers = @([ordered]@{ byteLength = $bytes.Length; uri = 'data:application/octet-stream;base64,' + [Convert]::ToBase64String($bytes) })
	bufferViews = @(
		[ordered]@{ buffer = 0; byteOffset = 0; byteLength = $positionBytes; target = 34962 },
		[ordered]@{ buffer = 0; byteOffset = $positionBytes; byteLength = $normalBytes; target = 34962 },
		[ordered]@{ buffer = 0; byteOffset = $positionBytes + $normalBytes; byteLength = $indexBytes; target = 34963 }
	)
	accessors = @(
		[ordered]@{ bufferView = 0; componentType = 5126; count = $positions.Count / 3; type = 'VEC3'; min = @(-$h, -$h, -$h); max = @($h, $h, $h) },
		[ordered]@{ bufferView = 1; componentType = 5126; count = $normals.Count / 3; type = 'VEC3' },
		[ordered]@{ bufferView = 2; componentType = 5123; count = $indices.Count; type = 'SCALAR' }
	)
}
$gltf | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $Out -Encoding utf8
Write-Host "wrote $Out ($($indices.Count / 3) triangles, $($bytes.Length) bytes of buffer)"
