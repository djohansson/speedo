# writes MovingInterior.gltf: things moving through a dim interior, for the indirect lighting of what moves (the probes'
# light, which a moving thing's voxels take while their entries of the surface cache are young: see "The probes,
# resolved where they show" in docs/notes/indirect-lighting.md). a closed room 12 m long, 4 m wide and 3 m tall, its
# walls, floor and ceiling boxes 0.3 m thick (no single sheets: a voxel then holds one side), lit through a skylight of
# 2 m by 2 m near its -x end alone, so that its +x end is lit by what bounces there; a pillar and a crate, static; and
# three things moving, one animation of 4 s that loops:
# - a white sphere (0.4 m radius) flying the room's length and back at 4.5 m/s, 1.2 m up: from under the skylight into
#   the dark end, a block of the finest level's voxels or more a frame;
# - a red cube (0.6 m) turning in place near the camera, once in 4 s: its voxels change while it stays in its blocks;
# - a green box (0.3 m) darting across the room 0.1 m over the floor, 6.4 m/s, a crossing in half a second.
# its camera stands at the dark end, looking along the room at the skylight. to compare frames, step the animation by a
# fixed time a frame (SPEEDO_ANIMATION_FRAME_SECONDS=0.015625: the loop is 256 frames). run it again after changing it:
# pwsh scripts/test-assets/movinginterior.ps1 (it writes resources/test-models/gltf/MovingInterior.gltf)
param([string] $Out = (Join-Path $PSScriptRoot '../../resources/test-models/gltf/MovingInterior.gltf'))

# a mesh being made: its positions, normals and indices
function NewMesh { @{ Positions = [System.Collections.Generic.List[float]]::new(); Normals = [System.Collections.Generic.List[float]]::new(); Indices = [System.Collections.Generic.List[uint16]]::new() } }

# a box between two corners, its six faces outward (counterclockwise seen from outside)
function AddBox($mesh, [double[]] $low, [double[]] $high)
{
	$faces = @(
		@{ N = @(1, 0, 0); U = @(0, 0, -1); V = @(0, 1, 0) }, @{ N = @(-1, 0, 0); U = @(0, 0, 1); V = @(0, 1, 0) },
		@{ N = @(0, 1, 0); U = @(1, 0, 0); V = @(0, 0, -1) }, @{ N = @(0, -1, 0); U = @(1, 0, 0); V = @(0, 0, 1) },
		@{ N = @(0, 0, 1); U = @(1, 0, 0); V = @(0, 1, 0) }, @{ N = @(0, 0, -1); U = @(-1, 0, 0); V = @(0, 1, 0) }
	)
	foreach ($face in $faces)
	{
		$base = $mesh.Positions.Count / 3
		foreach ($corner in @(@(-1, -1), @(1, -1), @(1, 1), @(-1, 1)))
		{
			for ($axis = 0; $axis -lt 3; $axis++)
			{
				# the face's corner: along each axis the low or the high side, by the sign of the normal, u or v there
				$sign = $face.N[$axis] + $corner[0] * $face.U[$axis] + $corner[1] * $face.V[$axis]
				$mesh.Positions.Add([float]$(if ($sign -gt 0) { $high[$axis] } else { $low[$axis] }))
				$mesh.Normals.Add([float]$face.N[$axis])
			}
		}
		foreach ($i in 0, 1, 2, 0, 2, 3) { $mesh.Indices.Add([uint16]($base + $i)) }
	}
}

# a sphere about the origin, in rings from its top
function AddSphere($mesh, [double] $radius, [int] $segments, [int] $rings)
{
	$base = $mesh.Positions.Count / 3
	for ($ring = 0; $ring -le $rings; $ring++)
	{
		$polar = [Math]::PI * $ring / $rings
		for ($segment = 0; $segment -le $segments; $segment++)
		{
			$around = 2.0 * [Math]::PI * $segment / $segments
			$n = @(([Math]::Sin($polar) * [Math]::Cos($around)), [Math]::Cos($polar), ([Math]::Sin($polar) * [Math]::Sin($around)))
			foreach ($value in $n) { $mesh.Positions.Add([float]($radius * $value)); $mesh.Normals.Add([float]$value) }
		}
	}
	for ($ring = 0; $ring -lt $rings; $ring++)
	{
		for ($segment = 0; $segment -lt $segments; $segment++)
		{
			$a = $base + $ring * ($segments + 1) + $segment
			$b = $a + $segments + 1
			# (none at the poles, where a quad's triangle has no area)
			if ($ring -gt 0) { foreach ($i in $a, ($a + 1), $b) { $mesh.Indices.Add([uint16]$i) } }
			if ($ring -lt $rings - 1) { foreach ($i in ($a + 1), ($b + 1), $b) { $mesh.Indices.Add([uint16]$i) } }
		}
	}
}

# the winding: every triangle's cross product along its vertices' normals
function CheckWinding($mesh, [string] $name)
{
	for ($t = 0; $t -lt $mesh.Indices.Count; $t += 3)
	{
		$p = @(0, 1, 2) | ForEach-Object { $k = $mesh.Indices[$t + $_] * 3; , @($mesh.Positions[$k], $mesh.Positions[$k + 1], $mesh.Positions[$k + 2]) }
		$a = @(($p[1][0] - $p[0][0]), ($p[1][1] - $p[0][1]), ($p[1][2] - $p[0][2]))
		$b = @(($p[2][0] - $p[0][0]), ($p[2][1] - $p[0][1]), ($p[2][2] - $p[0][2]))
		$cross = @(($a[1] * $b[2] - $a[2] * $b[1]), ($a[2] * $b[0] - $a[0] * $b[2]), ($a[0] * $b[1] - $a[1] * $b[0]))
		$dot = 0.0
		foreach ($corner in 0, 1, 2) { $k = $mesh.Indices[$t + $corner] * 3; $dot += $cross[0] * $mesh.Normals[$k] + $cross[1] * $mesh.Normals[$k + 1] + $cross[2] * $mesh.Normals[$k + 2] }
		if ($dot -le 0) { throw "$name's triangle $($t / 3) is wound inward" }
	}
}

# the room: inside it x -6 to 6, y 0 to 3, z -2 to 2; the boxes meet at their faces, none overlapping another's inside
# (x0, x1: the room's outside along x; z0, z1 along z; top: the ceiling's)
$wall = 0.3
$x0 = -6 - $wall; $x1 = 6 + $wall; $z0 = -2 - $wall; $z1 = 2 + $wall; $top = 3 + $wall
$walls = NewMesh
AddBox $walls @($x0, 0, $z0) @(-6, 3, $z1) # the -x end
AddBox $walls @(6, 0, $z0) @($x1, 3, $z1)  # the +x end
AddBox $walls @(-6, 0, $z0) @(6, 3, -2)    # the -z side
AddBox $walls @(-6, 0, 2) @(6, 3, $z1)     # the +z side
# the ceiling around the skylight (x -5 to -3, z -1 to 1)
AddBox $walls @($x0, 3, $z0) @(-5, $top, $z1)
AddBox $walls @(-3, 3, $z0) @($x1, $top, $z1)
AddBox $walls @(-5, 3, $z0) @(-3, $top, -1)
AddBox $walls @(-5, 3, 1) @(-3, $top, $z1)
$floor = NewMesh
AddBox $floor @($x0, (-$wall), $z0) @($x1, 0, $z1)
$props = NewMesh
AddBox $props @(-1.3, 0, 1.2) @(-0.7, 3, 1.8)   # a pillar by the +z side
AddBox $props @(2.2, 0, -1.9) @(3.0, 0.8, -1.1) # a crate by the -z side
$sphere = NewMesh
AddSphere $sphere 0.4 24 12
$cube = NewMesh
AddBox $cube @(-0.3, -0.3, -0.3) @(0.3, 0.3, 0.3)
$runner = NewMesh
AddBox $runner @(-0.15, -0.15, -0.15) @(0.15, 0.15, 0.15)
$meshes = @(
	@{ Name = 'walls'; Mesh = $walls; Color = @(0.75, 0.73, 0.7) }, @{ Name = 'floor'; Mesh = $floor; Color = @(0.5, 0.48, 0.45) },
	@{ Name = 'props'; Mesh = $props; Color = @(0.4, 0.5, 0.7) }, @{ Name = 'sphere'; Mesh = $sphere; Color = @(0.85, 0.85, 0.85) },
	@{ Name = 'cube'; Mesh = $cube; Color = @(0.85, 0.2, 0.15) }, @{ Name = 'runner'; Mesh = $runner; Color = @(0.25, 0.75, 0.3) }
)
foreach ($entry in $meshes) { CheckWinding $entry.Mesh $entry.Name }

# the animation: 4 s, linear. the sphere's and the runner's translations, the cube's rotation about +y (a quarter turn
# a key, the quaternion going on through -1 so that each step is the short way)
$sphereTimes = @(0.0, 2.0, 4.0)
$sphereKeys = @(-4.5, 1.2, 0.0, 4.5, 1.2, 0.0, -4.5, 1.2, 0.0)
$cubeTimes = @(0.0, 1.0, 2.0, 3.0, 4.0)
$cubeKeys = foreach ($quarter in 0..4) { $half = $quarter * [Math]::PI / 4.0; 0.0; [Math]::Sin($half); 0.0; [Math]::Cos($half) }
$runnerTimes = foreach ($key in 0..8) { 0.5 * $key }
$runnerKeys = foreach ($key in 0..8) { 1.0; 0.25; $(if ($key % 2 -eq 0) { -1.6 } else { 1.6 }) }

# the camera: at the dark end, looking along the room at the floor under the skylight (a node looks along its -z: turned
# about +y, then pitched)
$eye = @(5.4, 1.7, -1.5)
$target = @(-3.0, 0.9, 0.4)
$forward = @(($target[0] - $eye[0]), ($target[1] - $eye[1]), ($target[2] - $eye[2]))
$length = [Math]::Sqrt($forward[0] * $forward[0] + $forward[1] * $forward[1] + $forward[2] * $forward[2])
$yaw = [Math]::Atan2(-$forward[0], -$forward[2])
$pitch = [Math]::Asin($forward[1] / $length)
$cameraRotation = @(
	([Math]::Cos($yaw / 2) * [Math]::Sin($pitch / 2)), ([Math]::Sin($yaw / 2) * [Math]::Cos($pitch / 2)),
	(-[Math]::Sin($yaw / 2) * [Math]::Sin($pitch / 2)), ([Math]::Cos($yaw / 2) * [Math]::Cos($pitch / 2)))

# the buffer: each mesh's positions, normals and indices (padded to 4 bytes), then the animation's times and keys
$stream = [System.IO.MemoryStream]::new()
$writer = [System.IO.BinaryWriter]::new($stream)
$views = [System.Collections.Generic.List[object]]::new()
$accessors = [System.Collections.Generic.List[object]]::new()
function AddFloats([float[]] $values, [string] $type, [int] $target, [bool] $bounds)
{
	$offset = $stream.Position
	foreach ($value in $values) { $writer.Write([float]$value) }
	$view = [ordered]@{ buffer = 0; byteOffset = $offset; byteLength = $values.Count * 4 }
	if ($target -ne 0) { $view.target = $target }
	$views.Add($view)
	$width = @{ SCALAR = 1; VEC3 = 3; VEC4 = 4 }[$type]
	$accessor = [ordered]@{ bufferView = $views.Count - 1; componentType = 5126; count = $values.Count / $width; type = $type }
	if ($bounds)
	{
		$low = [float[]]::new($width)
		$high = [float[]]::new($width)
		for ($c = 0; $c -lt $width; $c++) { $low[$c] = $values[$c]; $high[$c] = $values[$c] }
		for ($i = 0; $i -lt $values.Count; $i++)
		{
			$c = $i % $width
			if ($values[$i] -lt $low[$c]) { $low[$c] = $values[$i] }
			if ($values[$i] -gt $high[$c]) { $high[$c] = $values[$i] }
		}
		$accessor.min = $low
		$accessor.max = $high
	}
	$accessors.Add($accessor)
	return $accessors.Count - 1
}
function AddIndices([uint16[]] $values)
{
	$offset = $stream.Position
	foreach ($value in $values) { $writer.Write([uint16]$value) }
	while ($stream.Position % 4 -ne 0) { $writer.Write([byte]0) }
	$views.Add([ordered]@{ buffer = 0; byteOffset = $offset; byteLength = $values.Count * 2; target = 34963 })
	$accessors.Add([ordered]@{ bufferView = $views.Count - 1; componentType = 5123; count = $values.Count; type = 'SCALAR' })
	return $accessors.Count - 1
}
$gltfMeshes = @()
$materials = @()
$triangles = 0
foreach ($entry in $meshes)
{
	$position = AddFloats $entry.Mesh.Positions.ToArray() 'VEC3' 34962 $true
	$normal = AddFloats $entry.Mesh.Normals.ToArray() 'VEC3' 34962 $false
	$index = AddIndices $entry.Mesh.Indices.ToArray()
	$gltfMeshes += [ordered]@{ name = $entry.Name; primitives = @([ordered]@{ attributes = [ordered]@{ POSITION = $position; NORMAL = $normal }; indices = $index; material = $materials.Count }) }
	$materials += [ordered]@{ name = $entry.Name; pbrMetallicRoughness = [ordered]@{ baseColorFactor = @($entry.Color[0], $entry.Color[1], $entry.Color[2], 1.0); metallicFactor = 0.0; roughnessFactor = 0.9 } }
	$triangles += $entry.Mesh.Indices.Count / 3
}
$sphereInput = AddFloats $sphereTimes 'SCALAR' 0 $true
$sphereOutput = AddFloats $sphereKeys 'VEC3' 0 $false
$cubeInput = AddFloats $cubeTimes 'SCALAR' 0 $true
$cubeOutput = AddFloats $cubeKeys 'VEC4' 0 $false
$runnerInput = AddFloats $runnerTimes 'SCALAR' 0 $true
$runnerOutput = AddFloats $runnerKeys 'VEC3' 0 $false
$writer.Flush()
$bytes = $stream.ToArray()

$gltf = [ordered]@{
	asset = [ordered]@{ version = '2.0'; generator = 'speedo test assets' }
	scene = 0
	scenes = @([ordered]@{ name = 'MovingInterior'; nodes = @(0, 1, 2, 3, 4, 5, 6) })
	nodes = @(
		[ordered]@{ name = 'walls'; mesh = 0 }, [ordered]@{ name = 'floor'; mesh = 1 }, [ordered]@{ name = 'props'; mesh = 2 },
		[ordered]@{ name = 'sphere'; mesh = 3; translation = @(-4.5, 1.2, 0.0) },
		[ordered]@{ name = 'cube'; mesh = 4; translation = @(2.4, 0.7, 0.7) },
		[ordered]@{ name = 'runner'; mesh = 5; translation = @(1.0, 0.25, -1.6) },
		[ordered]@{ name = 'camera'; camera = 0; translation = $eye; rotation = $cameraRotation }
	)
	cameras = @([ordered]@{ type = 'perspective'; perspective = [ordered]@{ yfov = 0.9; znear = 0.05; zfar = 100.0 } })
	meshes = $gltfMeshes
	materials = $materials
	animations = @([ordered]@{
			name = 'moving'
			samplers = @(
				[ordered]@{ input = $sphereInput; output = $sphereOutput; interpolation = 'LINEAR' },
				[ordered]@{ input = $cubeInput; output = $cubeOutput; interpolation = 'LINEAR' },
				[ordered]@{ input = $runnerInput; output = $runnerOutput; interpolation = 'LINEAR' }
			)
			channels = @(
				[ordered]@{ sampler = 0; target = [ordered]@{ node = 3; path = 'translation' } },
				[ordered]@{ sampler = 1; target = [ordered]@{ node = 4; path = 'rotation' } },
				[ordered]@{ sampler = 2; target = [ordered]@{ node = 5; path = 'translation' } }
			)
		})
	buffers = @([ordered]@{ byteLength = $bytes.Length; uri = 'data:application/octet-stream;base64,' + [Convert]::ToBase64String($bytes) })
	bufferViews = $views.ToArray()
	accessors = $accessors.ToArray()
}
$gltf | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $Out -Encoding utf8
Write-Host "wrote $Out ($triangles triangles, $($bytes.Length) bytes of buffer)"
