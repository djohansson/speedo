// the traced shadows: a ray per pixel of the pre-pass's depth and traced light, toward a point of the light (its disk,
// see LightData::size) through the model's BVHs, where the shadow atlas isn't sure (the Shadow Classify and Shadow Trace
// passes, ComputeShadowClassify and ComputeShadowTrace), its visibility filtered over the pixels around by its
// penumbra's width (Shadow Denoise, ComputeShadowDenoiseX and Y): what the main pass's opaque fragments take in place of
// the shadow atlas (ShadowFactor). a part of shaders.slang, which includes it after gi.slang

// a pixel's surface as the pre-pass drew it: where it is, its geometry normal (toward the side seen), whether it takes
// light from behind (see PrepassOutput), and how wide a pixel is there
struct ShadowSurface
{
	float3 position;
	float3 normal;
	bool fromBehind;
	float metersPerPixel;
};

float3 ShadowPosition(ViewData view, float2 pixel, float depth)
{
	float2 ndc = (pixel + 0.5 - view.viewport.xy) / view.viewport.zw * 2.0 - 1.0;
	float4 position = mul(view.inverseViewProjection, float4(ndc, depth, 1.0));
	return position.xyz / position.w;
}

// false where nothing was drawn
bool ShadowSurfaceAt(int2 pixel, ViewData view, out ShadowSurface surface)
{
	surface = (ShadowSurface)0;
	float depth = gTextures[SHADER_TYPES_MAIN_DEPTH_TEXTURE].Load(int3(pixel, 0)).r;
	if (depth >= 1.0)
		return false;
	float4 normal = gTextures[SHADER_TYPES_PREPASS_NORMAL_TEXTURE].Load(int3(pixel, 0));
	surface.position = ShadowPosition(view, float2(pixel), depth);
	surface.normal = normalize(normal.xyz * 2.0 - 1.0);
	surface.fromBehind = normal.w > 0.5;
	surface.metersPerPixel = max(distance(surface.position, ShadowPosition(view, float2(pixel) + float2(1.0, 0.0), depth)), 1e-6);
	return true;
}

uint ShadowHash(uint2 v)
{
	// pcg2d (Jarzynski and Olano)
	v = v * 1664525u + 1013904223u;
	v.x += v.y * 1664525u;
	v.y += v.x * 1664525u;
	v ^= v >> 16u;
	v.x += v.y * 1664525u;
	v.y += v.x * 1664525u;
	v ^= v >> 16u;
	return v.x ^ v.y;
}

// the point of the light's disk (unit radius) a pixel's ray goes to, one of 16 over the 4x4 tile it is in: a band of
// the radius (of equal area) and a quarter of the circle, from two orthogonal latin squares over GF(4) (band x ^ 2y,
// quarter x ^ y), so that every row, column and aligned 2x2 block of the tile has all four bands and every row and
// column all four quarters (a quarter and its xor with 1 opposite); then within them, the square of the radius in
// sixteenths 4 band + quarter and the angle 4 quarter + band, a (0,2,2)-net: every sixteenth of the disk once. turned
// per tile, so that the filter's reach over a few tiles meets every part of the disk and no pattern repeats. by the
// index of a Vogel spiral, each row of a tile had a band of the radius of its own (its first the middle of the disk,
// its last the rim), so each row's penumbra was as wide as its band's, which the filter along the rows kept: a
// narrow penumbra came out in steps along its rows (its mean error against the reference was a little lower, 32 of 255
// against 34 on ShadowScene's narrow edges, 7.6 against 8 on its wide ones: steps show where noise doesn't). seed: the
// tiles' turns, for a pattern other than the shadows' (the near field's, see GiNearDirection)
float2 ShadowDiskPoint(int2 pixel, uint seed = 0u)
{
	uint x = uint(pixel.x) & 3u;
	uint y = uint(pixel.y) & 3u;
	uint band = x ^ ((0x1320u >> (y * 4u)) & 3u);
	uint quarter = (0x3120u >> ((x ^ y) * 4u)) & 3u;
	float turn = float(ShadowHash((uint2(pixel) >> 2u) ^ uint2(seed, seed * 3u)) & 0xffffu) / 65536.0 * 6.28318530718;
	float radius = sqrt((float(band * 4u + quarter) + 0.5) / 16.0);
	float angle = (float(quarter * 4u + band) + 0.5) / 16.0 * 6.28318530718 + turn;
	return radius * float2(cos(angle), sin(angle));
}

void ShadowBasis(float3 n, out float3 t, out float3 b)
{
	// Duff et al.'s branchless basis
	float s = n.z >= 0.0 ? 1.0 : -1.0;
	float a = -1.0 / (s + n.z);
	float c = n.x * n.y * a;
	t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
	b = float3(c, s + n.y * n.y * a, -n.y);
}

// whether something is between origin and a distance along direction: the static BVH, then what moves (any hit; alpha
// masks cut out as the main pass's are), and the distance to it (or -1 if nothing is)
float ShadowOccluder(float3 origin, float3 direction, float tmax, bool moving)
{
	BvhHit hit = BvhTraverse(gBvhNodes, gBvhTriangles, origin, direction, tmax, true, GiAlphaMask());
	if (BvhHitSomething(hit))
		return hit.t;
	if (hit.capped)
		return 0.0; // something its traversal couldn't get through
	if (moving)
	{
		uint frameIndex = gPushConstants.frameIndex;
		BvhHit movingHit = BvhTraverse2(gBvhMovingNodes[frameIndex], gBvhMovingTriangles[frameIndex], origin, direction, tmax, true, GiAlphaMask());
		if (BvhHitSomething(movingHit))
			return movingHit.t;
		if (movingHit.capped)
			return 0.0;
	}
	return -1.0;
}

// how far from a shadow view's light a depth of it is, along its view (see ShadowData::params)
float ShadowViewDistance(ShadowData view, float depth)
{
	float n = view.params.z;
	float f = view.params.w;
	return view.params.y > 0.0 ? n * f / max(f - depth * (f - n), 1e-12) : n + depth * (f - n);
}

// half the width of a light's penumbra a blocker d before a point makes, s from the light: a disk of angular radius a,
// d tan(a); a sphere of radius r, r d / (s - d)
float ShadowPenumbraHalfWidth(LightData light, float d, float s)
{
	return light.type == LIGHT_TYPE_DIRECTIONAL ? d * tan(light.size) : light.size * d / max(s - d, 1e-3);
}

// what the shadow atlas says of a pixel for a light: 1 lit, 0 shadowed, else -1 (unsure: traced). sure where its filter
// is all lit or all shadowed and no texel around (24 taps up to 24 texels, more of them near the pixel) says otherwise
// within the reach of the penumbra it would make: a blocking texel within half the penumbra's width its depth gives
// (plus 2 texels, the filter's and the bias's), of a lit pixel; a texel past the blocker within half of its own
// penumbra's, of a shadowed one. so only a band about as wide as the penumbrae around the shadows' edges is traced
// (the real sun's, a centimetre or two from what is near). the texels are compared with the pixel's tangent plane where
// they are (a surface sloping toward the light isn't its own blocker). 24 texels of the nearest cascade (about a
// centimetre each near the eye) see a penumbra cast from up to 50 m away; a contact shadow finer than the atlas's bias
// is lost, as it is without the rays
float ShadowMapClassify(uint lightIndex, LightData light, ShadowSurface surface, float3 toLight, int2 pixel)
{
	ShadowMapPlace place;
	uint placed = PlaceInShadowMap(lightIndex, light, surface.position, surface.normal, toLight, place);
	if (placed == kShadowMapNearer)
		return 1.0;
	if (placed != kShadowMapPlaced)
		return -1.0;
	float filtered = ShadowMapFilter(place.uv, place.depth);
	if (filtered > 0.0 && filtered < 1.0)
		return -1.0;

	Texture2D atlas = gTextures[SHADER_TYPES_SHADOW_ATLAS_TEXTURE];
	float2 atlasSize;
	atlas.GetDimensions(atlasSize.x, atlasSize.y);
	float2 texel = 1.0 / atlasSize;
	float toLightDistance = light.type == LIGHT_TYPE_DIRECTIONAL ? 1e9 : length(light.positionRange.xyz - surface.position);
	float texelSize = ShadowTexelSize(place.view, toLightDistance);
	float2 gradient = ShadowReceiverGradient(place, surface.position, surface.normal, texelSize);
	float slack = abs(dot(abs(gradient), texel)) * 2.0 + 1e-5; // a texel's depth of slack, as the filter's normal offset
	float2 low = place.view.atlasRect.xy + texel;
	float2 high = place.view.atlasRect.xy + place.view.atlasRect.zw - texel;

	// the filter's verdict where the pixel itself is (without the normal offset), its 4x4 texels compared with its tangent
	// plane within a texel's slope: where they disagree, the verdict is the bias's, and the rays decide. a contact shadow
	// within the offset's reach (Bistro's cornice band under its ledge, the user's pose 2991.8064,394.9334,1467.5278,-0.016,5.767:
	// the offset lookup lit and sure, the ledge's texels over the band itself nearer than its plane) came out in the
	// atlas's staircase, a sawtooth of teeth along the ledge; a concave corner's other surface (CornellBox's back wall at
	// its side wall and floor, the user's pose 0.0465,-0.0665,1.2875,-1.722,0.393: the filter, comparing the texels with
	// the pixel's own depth, shadowed and sure, the side wall's texels within a texel's slope of the back wall's plane) a
	// dark line a texel wide along every edge
	{
		float4 clip = mul(place.view.viewProjection, float4(surface.position, 1.0));
		float3 ndc = clip.xyz / max(clip.w, 1e-6);
		float2 uvHere = place.view.atlasRect.xy + (ndc.xy * 0.5 + 0.5) * place.view.atlasRect.zw;
		float tight = abs(dot(abs(gradient), texel)) + 1e-5;
		int2 base = int2(floor(uvHere * atlasSize - 0.5));
		uint blocked = 0u;
		for (uint texelIt = 0; texelIt < 4u; texelIt++)
		{
			float2 uvAt = clamp((float2(base + int2(texelIt & 1u, texelIt >> 1u)) + 0.5) * texel, low, high);
			float stored = atlas.Load(int3(int2(uvAt * atlasSize), 0)).r;
			blocked += stored < ndc.z + dot(gradient, uvAt - uvHere) - tight ? 1u : 0u;
		}
		if (filtered >= 1.0 ? blocked != 0u : blocked != 4u)
			return -1.0;
	}

	float planeDistance = ShadowViewDistance(place.view, place.depth);
	// a shadowed pixel's own blocker: the reach of its penumbra
	float ownReach = 0.0;
	if (filtered <= 0.0)
	{
		float stored = atlas.Load(int3(int2(place.uv * atlasSize), 0)).r;
		ownReach = ShadowPenumbraHalfWidth(light, max(planeDistance - ShadowViewDistance(place.view, stored), 0.0), toLightDistance);
	}
	float margin = 2.0 * texelSize;
	float turn = float(ShadowHash(uint2(pixel)) & 0xffffu) / 65536.0 * 6.28318530718;
	for (uint tapIt = 0; tapIt < 24; tapIt++)
	{
		float radius = 24.0 * pow((float(tapIt) + 0.5) / 24.0, 1.5);
		float angle = float(tapIt) * 2.39996323 + turn;
		float2 uv = clamp(place.uv + radius * float2(cos(angle), sin(angle)) * texel, low, high);
		float stored = atlas.Load(int3(int2(uv * atlasSize), 0)).r;
		float plane = place.depth + dot(gradient, uv - place.uv);
		float reach = length((uv - place.uv) * atlasSize) * texelSize;
		bool blocks = stored < plane - slack;
		if (filtered >= 1.0 && blocks)
		{
			float d = max(ShadowViewDistance(place.view, plane) - ShadowViewDistance(place.view, stored), 0.0);
			if (reach <= ShadowPenumbraHalfWidth(light, d, toLightDistance) + margin)
				return -1.0;
		}
		if (filtered <= 0.0 && !blocks && reach <= ownReach + margin)
			return -1.0;
	}
	return filtered >= 1.0 ? 1.0 : 0.0;
}

// a shadow ray from a surface toward a point of its light's disk (unit radius; see LightData::size), off the surface on
// the side toward the light by bias, up to the light: the distance to what is in the way, or -1 (see ShadowOccluder)
float ShadowRay(LightData light, ShadowSurface surface, float3 toLight, float side, float bias, float2 disk, bool moving)
{
	float3 t;
	float3 b;
	float3 direction;
	float tmax;
	ShadowBasis(toLight, t, b);
	if (light.type == LIGHT_TYPE_DIRECTIONAL)
	{
		direction = normalize(toLight + tan(light.size) * (disk.x * t + disk.y * b));
		tmax = 1e9;
	}
	else
	{
		float3 target = light.positionRange.xyz + light.size * (disk.x * t + disk.y * b);
		direction = target - surface.position;
		tmax = length(direction);
		direction /= max(tmax, 1e-6);
		tmax = max(tmax - light.size - bias, 0.0);
	}
	float3 origin = surface.position + surface.normal * (side >= 0.0 ? bias : -bias);
	return ShadowOccluder(origin, direction, tmax, moving);
}

// a pixel's traced shadows: per traced light, 1 where its ray toward the point of the light its place in its tile says
// (ShadowDiskPoint) gets there, 0 where something is in the way (SHADER_TYPES_SHADOW_RAW_TEXTURE), and that something's
// distance from the pixel, for the penumbra's width (SHADER_TYPES_SHADOW_BLOCKER_TEXTURE: 0 where the ray got through or
// the shadow atlas decided, -1 where the light doesn't reach the pixel at all, which the filter leaves out). where the
// atlas is sure (ShadowMapClassify), no ray; where it isn't, the rays only with traceUnsure, else false is returned (the
// pixel is to be traced, see ComputeShadowClassify). the ray starts off the surface on the light's side (the side its
// query goes to: a leaf lit from behind pushed toward the viewer was behind itself), by a pixel and a half, more far
// from the eye, against the depth's and the BVH's rounding
bool ShadowTracePixel(int2 pixel, bool traceUnsure, out float4 lit, out float4 blocker)
{
	lit = 1.0;
	blocker = -1.0;
	ViewData view;
	ShadowSurface surface;
	if (!ViewAt(pixel, view) || !ShadowSurfaceAt(pixel, view, surface))
		return true;

	ShadowTraceData trace = gShadowTrace[gPushConstants.frameIndex][0];
	float2 disk = ShadowDiskPoint(pixel);
	float bias = max(1.5 * surface.metersPerPixel, 1e-4 * distance(surface.position, view.eyePosition.xyz));
	for (uint slot = 0; slot < SHADER_TYPES_SHADOW_TRACE_SLOT_COUNT; slot++)
	{
		uint lightIndex = trace.lights[slot];
		if (lightIndex == SHADER_TYPES_NO_SHADOW)
			continue;
		LightData light = gLights[lightIndex];
		float3 toLight;
		if (LightFalloff(light, surface.position, toLight) <= 0.0)
			continue;
		// facing away: shadowed by its own surface (a normal map may face the light where the geometry doesn't), and
		// left out of the filter (whose average across the terminator would darken the lit side)
		float side = dot(surface.normal, toLight);
		if (side <= 0.0 && !surface.fromBehind)
		{
			lit[slot] = 0.0;
			continue;
		}

		// where the shadow atlas is sure, no ray (see ShadowMapClassify)
		if (trace.mapped != 0u)
		{
			float mapped = ShadowMapClassify(lightIndex, light, surface, toLight, pixel);
			if (mapped >= 0.0)
			{
				lit[slot] = mapped;
				blocker[slot] = 0.0;
				continue;
			}
		}
		if (!traceUnsure)
			return false;

		bool moving = trace.movingBvh != 0u;
		if (slot == 0u && gPushConstants.debugView == DEBUG_VIEW_TRACED_SHADOW_REFERENCE)
		{
			// the reference: rays to a Vogel spiral of points over the whole disk, turned per pixel
			float turn = float(ShadowHash(uint2(pixel)) & 0xffffu) / 65536.0 * 6.28318530718;
			float through = 0.0;
			for (uint rayIt = 0u; rayIt < SHADER_TYPES_SHADOW_REFERENCE_RAY_COUNT; rayIt++)
			{
				float radius = sqrt((float(rayIt) + 0.5) / float(SHADER_TYPES_SHADOW_REFERENCE_RAY_COUNT));
				float angle = float(rayIt) * 2.39996323 + turn;
				float2 point = radius * float2(cos(angle), sin(angle));
				through += ShadowRay(light, surface, toLight, side, bias, point, moving) >= 0.0 ? 0.0 : 1.0;
			}
			lit[slot] = through / float(SHADER_TYPES_SHADOW_REFERENCE_RAY_COUNT);
			blocker[slot] = 0.0;
			continue;
		}
		float occluder = ShadowRay(light, surface, toLight, side, bias, disk, moving);
		lit[slot] = occluder >= 0.0 ? 0.0 : 1.0;
		blocker[slot] = max(occluder, 0.0);
	}
	return true;
}

// the Shadow Classify pass: the pixels the shadow atlas is sure of for every traced light (see ShadowTracePixel) written,
// the others listed for the Shadow Trace pass (gShadowTraceList: a count, then the pixels, x and y in 16 bits each): a
// group of threads tracing the few pixels along shadows' edges among those it has waits for the slowest of them (Bistro's
// 136000 rays a frame, 4% of its pixels, cost as much as 450000 there), the listed ones are traced by every thread
// and the 8x8 tiles within reach of a listed pixel's flagged (gShadowTraceTiles, see ShadowTileFlag): the Shadow Denoise
// filters only there (what the atlas decided is all lit or all shadowed elsewhere, the shadows' edges being traced)
groupshared uint gShadowGroupListed;

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeShadowClassify(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
	RWTexture2D visibility = gRWTextures[SHADER_TYPES_SHADOW_RAW_RW_TEXTURE];
	uint2 extent;
	visibility.GetDimensions(extent.x, extent.y);
	if (groupIndex == 0u)
		gShadowGroupListed = 0u;
	GroupMemoryBarrierWithGroupSync();
	int2 pixel = int2(dispatchThreadId.xy);
	if (all(dispatchThreadId.xy < extent))
	{
		float4 lit;
		float4 blocker;
		if (ShadowTracePixel(pixel, false, lit, blocker))
		{
			visibility[pixel] = lit;
			gRWTextures[SHADER_TYPES_SHADOW_BLOCKER_RW_TEXTURE][pixel] = blocker;
		}
		else
		{
			uint entry;
			InterlockedAdd(gShadowTraceList[0], 1u, entry);
			gShadowTraceList[1u + entry] = uint(pixel.x) | (uint(pixel.y) << 16u);
			gShadowGroupListed = 1u;
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (groupIndex == 0u && gShadowGroupListed != 0u)
	{
		// the filter reaches 16 pixels: 2 tiles each way
		uint2 tiles = (extent + 7u) / 8u;
		for (int y = -2; y <= 2; y++)
			for (int x = -2; x <= 2; x++)
			{
				int2 tile = int2(groupId.xy) + int2(x, y);
				if (all(tile >= 0) && all(tile < int2(tiles)))
					gShadowTraceTiles[uint(tile.y) * tiles.x + uint(tile.x)] = 1u;
			}
	}
}

// whether a pixel's tile is within reach of a traced one (see ComputeShadowClassify)
bool ShadowTileFlag(int2 pixel, uint2 extent)
{
	uint2 tiles = (extent + 7u) / 8u;
	uint2 tile = uint2(pixel) / 8u;
	return gShadowTraceTiles[tile.y * tiles.x + tile.x] != 0u;
}

// the Shadow Trace pass: the listed pixels (see ComputeShadowClassify), SHADER_TYPES_SHADOW_TRACE_THREAD_COUNT threads
// taking them in turn
[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeShadowTrace(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint count = gShadowTraceList[0];
	for (uint entry = dispatchThreadId.x; entry < count; entry += SHADER_TYPES_SHADOW_TRACE_THREAD_COUNT)
	{
		uint packed = gShadowTraceList[1u + entry];
		int2 pixel = int2(packed & 0xffffu, packed >> 16u);
		float4 lit;
		float4 blocker;
		ShadowTracePixel(pixel, true, lit, blocker);
		gRWTextures[SHADER_TYPES_SHADOW_RAW_RW_TEXTURE][pixel] = lit;
		gRWTextures[SHADER_TYPES_SHADOW_BLOCKER_RW_TEXTURE][pixel] = blocker;
	}
}

// half the width of each traced light's penumbra at a pixel (meters along its surface): from the distance to its
// blocker (a ray of its that met one: the shadow it is in), else from the mean of those the rays around met (16 taps up
// to 16 pixels away: a lit pixel next to a shadow blurs with its edge); 0 for a slot without a light or of a light
// without a size (its rays all agree), and -1 where no ray around met a blocker, which may be in a penumbra still (its
// pixels' rays lit by chance, the taps' places being fixed: ShadowScene's 2 degree sun left 9 pixels of its penumbra
// unfiltered, single rays as bright as the light, Bistro's street 124). the
// nearest of them rather than their mean was 2.6 of 255 further from the reference in Bistro's street, the mean of
// them all, the pixel's own among them, the same
float4 ShadowPenumbraHalfWidths(int2 pixel, ShadowSurface surface, ShadowTraceData trace, int2 extent)
{
	Texture2D blockers = gTextures[SHADER_TYPES_SHADOW_BLOCKER_TEXTURE];
	float4 own = blockers.Load(int3(pixel, 0));
	float4 sum = 0.0;
	float4 count = 0.0;
	for (uint tapIt = 0; tapIt < 16u; tapIt++)
	{
		float radius = 16.0 * sqrt((float(tapIt) + 0.5) / 16.0);
		float angle = float(tapIt) * 2.39996323;
		int2 at = clamp(pixel + int2(round(radius * float2(cos(angle), sin(angle)))), int2(0), extent - 1);
		float4 blocker = blockers.Load(int3(at, 0));
		sum += select(blocker > 0.0, blocker, float4(0.0));
		count += select(blocker > 0.0, float4(1.0), float4(0.0));
	}
	float4 around = select(count > 0.0, sum / max(count, 1.0), float4(0.0));
	float4 distance = select(own > 0.0, own, around);
	float4 halfWidth = 0.0;
	for (uint slot = 0; slot < SHADER_TYPES_SHADOW_TRACE_SLOT_COUNT; slot++)
	{
		uint lightIndex = trace.lights[slot];
		if (lightIndex == SHADER_TYPES_NO_SHADOW)
			continue;
		LightData light = gLights[lightIndex];
		if (light.size > 0.0)
			halfWidth[slot] = distance[slot] > 0.0
				? ShadowPenumbraHalfWidth(light, distance[slot], length(light.positionRange.xyz - surface.position))
				: -1.0;
	}
	return halfWidth;
}

// the meters a pixel's step covers along its surface (where the ray through the pixel a step away meets the surface's
// tangent plane): more than a pixel's width facing the eye where the surface is seen at a slant, at most 16 times that
float ShadowFootprint(ViewData view, int2 pixel, int2 step, ShadowSurface surface)
{
	float2 at = float2(pixel + step);
	float3 near = ShadowPosition(view, at, 0.0);
	float3 direction = normalize(ShadowPosition(view, at, 0.5) - near);
	float slant = dot(direction, surface.normal);
	float most = 16.0 * surface.metersPerPixel;
	if (abs(slant) < 1e-6)
		return most;
	float t = dot(surface.position - near, surface.normal) / slant;
	return clamp(distance(near + direction * t, surface.position), surface.metersPerPixel, most);
}

// a pass of the Shadow Denoise: the visibility (or the rows' result) averaged along the row (or column), weighed by a
// gaussian of the distance along the surface whose deviation is a third of each traced light's penumbra's half width
// (ShadowPenumbraHalfWidths) and at least the step's footprint on the surface (ShadowFootprint: a surface seen at a
// slant is blurred no further across than along it), up to 16 pixels away, and by how much the neighbor is on the
// pixel's surface (its distance off the pixel's tangent plane, against a pixel's width, and how its normal agrees),
// leaving out where the light doesn't reach. against the reference (DEBUG_VIEW_TRACED_SHADOW_REFERENCE, 256 rays a
// pixel), ShadowScene's penumbrae under a sun of 2 degrees were 8 of 255 off on average: at a deviation of 0.7 of the
// half width, in pixels of the step facing the eye, as it was, 15 (a ramp of the penumbra's width widened by three
// fifths, sqrt(1/12 + 1/8) over sqrt(1/12), where a third widens it by a sixth), and at a quarter or two fifths of it
// further off than a third. the pixel's floor: the disk's points differ from pixel to pixel, and a sub-pixel penumbra
// filtered over less (by its half width) was as jagged as its rays. what is left, an edge of the real sun wobbling by
// a pixel from tile to tile (the narrow penumbrae 34 of 255 off), takes more rays than a pixel's one
float4 ShadowDenoise(int2 pixel, int2 step, uint source, uint target)
{
	RWTexture2D result = gRWTextures[target];
	uint2 size;
	result.GetDimensions(size.x, size.y);
	int2 extent = int2(size);
	Texture2D input = gTextures[source];
	Texture2D blockers = gTextures[SHADER_TYPES_SHADOW_BLOCKER_TEXTURE];
	// the rays' and the reference's debug views: passed through to the mask, which the main pass reads (the raw
	// visibility's memory is the mask's, being a transient whose last use is the filter's)
	uint debugView = gPushConstants.debugView;
	if (!ShadowTileFlag(pixel, size) || debugView == DEBUG_VIEW_TRACED_SHADOW_RAW || debugView == DEBUG_VIEW_TRACED_SHADOW_REFERENCE)
		return input.Load(int3(pixel, 0));
	ViewData view;
	ShadowSurface center;
	if (!ViewAt(pixel, view) || !ShadowSurfaceAt(pixel, view, center))
		return 1.0;
	ShadowTraceData trace = gShadowTrace[gPushConstants.frameIndex][0];
	float4 halfWidth = ShadowPenumbraHalfWidths(pixel, center, trace, extent);
	float footprint = ShadowFootprint(view, pixel, step, center);
	halfWidth = select(halfWidth < 0.0, float4(footprint), halfWidth); // unknown: the floor
	float4 sigma = max(halfWidth / 3.0, footprint);
	// a penumbra narrower than a tenth of a pixel is as sharp as the rays have it (they all agree), and a pixel the light
	// doesn't reach (facing away from it) keeps its own: its neighbors on its surface are left out, and those beyond its
	// edge alone lit it
	bool4 filtered = halfWidth >= 0.1 * footprint && blockers.Load(int3(pixel, 0)) >= 0.0;
	float4 reaches = select(filtered, min(3.0 * sigma / footprint, 16.0), float4(0.0));
	if (!any(filtered))
		return input.Load(int3(pixel, 0));
	int reach = int(ceil(max(max(reaches.x, reaches.y), max(reaches.z, reaches.w))));
	float4 inverseTwoSigma2 = 1.0 / (2.0 * sigma * sigma);
	float4 total = 0.0;
	float4 weights = 0.0;
	float2 low = view.viewport.xy;
	float2 high = view.viewport.xy + view.viewport.zw;
	for (int offset = -reach; offset <= reach; offset++)
	{
		int2 at = pixel + step * offset;
		if (any(float2(at) < low) || any(float2(at) >= high))
			continue;
		float depth = gTextures[SHADER_TYPES_MAIN_DEPTH_TEXTURE].Load(int3(at, 0)).r;
		if (depth >= 1.0)
			continue;
		float3 position = ShadowPosition(view, float2(at), depth);
		float3 normal = gTextures[SHADER_TYPES_PREPASS_NORMAL_TEXTURE].Load(int3(at, 0)).xyz * 2.0 - 1.0;
		float3 apart = position - center.position;
		float plane = abs(dot(apart, center.normal)) / (2.0 * center.metersPerPixel);
		float facing = saturate(dot(normal, center.normal) * rsqrt(max(dot(normal, normal), 1e-8)));
		float similar = exp(-plane) * pow(facing, 8.0);
		float4 reached = select(blockers.Load(int3(at, 0)) >= 0.0, float4(1.0), float4(0.0));
		float4 w = similar * reached * select(abs(float(offset)) <= reaches, exp(-dot(apart, apart) * inverseTwoSigma2), float4(0.0));
		total += w * input.Load(int3(at, 0));
		weights += w;
	}
	float4 own = input.Load(int3(pixel, 0));
	return select(filtered && weights > 0.0, total / max(weights, 1e-6), own);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeShadowDenoiseX(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint2 extent;
	gRWTextures[SHADER_TYPES_SHADOW_ROWS_RW_TEXTURE].GetDimensions(extent.x, extent.y);
	if (any(dispatchThreadId.xy >= extent))
		return;
	int2 pixel = int2(dispatchThreadId.xy);
	gRWTextures[SHADER_TYPES_SHADOW_ROWS_RW_TEXTURE][pixel] =
		ShadowDenoise(pixel, int2(1, 0), SHADER_TYPES_SHADOW_RAW_TEXTURE, SHADER_TYPES_SHADOW_ROWS_RW_TEXTURE);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeShadowDenoiseY(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint2 extent;
	gRWTextures[SHADER_TYPES_SHADOW_MASK_RW_TEXTURE].GetDimensions(extent.x, extent.y);
	if (any(dispatchThreadId.xy >= extent))
		return;
	int2 pixel = int2(dispatchThreadId.xy);
	gRWTextures[SHADER_TYPES_SHADOW_MASK_RW_TEXTURE][pixel] =
		ShadowDenoise(pixel, int2(0, 1), SHADER_TYPES_SHADOW_ROWS_TEXTURE, SHADER_TYPES_SHADOW_MASK_RW_TEXTURE);
}
