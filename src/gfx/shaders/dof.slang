// the camera's depth of field (see LensData::dof and the DOF passes in gfx/renderer.cpp): what a real lens focuses at a
// distance leaves blurred before and beyond it, each point of the frame spread over its circle of confusion on the
// sensor (the thin lens: the aperture's diameter times how far the point's image falls from the sensor over the image's
// distance), which the aperture's blades shape and the lens's vignetting clips at the edges of the field (the cat's
// eye), the red's and the blue's circles a little apart from the green's (the longitudinal chromatic aberration). the
// frame is reduced to half the output's size with its circles (ComputeDofPrepare), each tile's largest circle found
// (ComputeDofTiles, ComputeDofTilesDilate: the gather's reach), gathered (ComputeDofGather: a scatter as gather, each
// tap counting where its circle covers the pixel, a sharp pixel in front hiding a blurred one behind it), and the
// frame recomposed at the output's size (ComputeDofComposite), which the Lens pass takes in place of the frame. a part
// of shaders.slang, included after lens.slang

static const uint kDofTapCount = 48u;
static const float kDofGoldenAngle = 2.39996323;

// interleaved gradient noise (Jimenez 2014), 0 to 1 per pixel, by which the gather's spiral is turned
float DofNoise(float2 pixel)
{
	return frac(52.9829189 * frac(0.06711056 * pixel.x + 0.00583715 * pixel.y));
}

// the distance along the view (mm) of what is at a depth of a view, and the radius of the circle of confusion of a point
// that far (signed, in mm: positive beyond the focus, negative before it) for a channel whose focal length is shift from
// the green's: the sensor at the green's image of the focus, the point's image at its own; the aperture's diameter times
// how far the image falls from the sensor over the image's distance is the circle's diameter, halved (the passes after
// take the circles as radii: they spread over twice the thin lens's circle before, 2026-11-06)
float DofDistanceMm(ViewData view, float depth)
{
	return UpscaleViewDepth(view, depth) * gPushConstants.metersPerUnit * 1000.0;
}

float DofCircleMm(LensData lens, float distanceMm, float shift)
{
	float focal = lens.focalLength;
	float focusImage = 1.0 / max(1.0 / focal - 1.0 / max(lens.focusDistance, 1.01 * focal), 1e-6);
	float pointFocal = focal + shift;
	float pointImage = 1.0 / max(1.0 / pointFocal - 1.0 / max(distanceMm, 1.01 * pointFocal), 1e-6);
	return 0.5 * lens.apertureDiameter * (focusImage - pointImage) / pointImage;
}

// the circles' radii of the three channels in the output's pixels, clamped to SHADER_TYPES_DOF_MAX_COC (the green's
// sign)
float3 DofCircles(LensData lens, float distanceMm, float mmPerPixel)
{
	float3 circles = float3(
		DofCircleMm(lens, distanceMm, lens.focusShiftRed), DofCircleMm(lens, distanceMm, 0.0), DofCircleMm(lens, distanceMm, lens.focusShiftBlue));
	return clamp(circles / max(mmPerPixel, 1e-6), -SHADER_TYPES_DOF_MAX_COC, SHADER_TYPES_DOF_MAX_COC);
}

// the view, its viewport in the output's pixels and the sensor's mm per pixel at an output pixel; false outside the
// views
bool DofViewAt(float2 outputPixel, uint2 outputSize, LensData lens, out ViewData view, out float4 viewport, out float mmPerPixel)
{
	viewport = 0.0;
	mmPerPixel = 1.0;
	if (!UpscaleViewAtUv((outputPixel + 0.5) / float2(outputSize), view))
		return false;
	viewport = LensOutputViewport(view, outputSize);
	float2 mm = LensSensorMmPerPixel(lens, viewport);
	mmPerPixel = max(mm.x, mm.y);
	return true;
}

// the depth at an output pixel: the main depth's at the render pixel under it
float DofDepthAt(float2 outputPixel, uint2 outputSize)
{
	int2 renderSize = UpscaleRenderSize();
	int2 at = clamp(int2(outputPixel * float2(renderSize) / float2(outputSize)), int2(0, 0), renderSize - 1);
	return gTextures[SHADER_TYPES_MAIN_DEPTH_TEXTURE].Load(int3(at, 0)).r;
}

// the largest circle of each tile of SHADER_TYPES_DOF_TILE_SIZE output pixels (unsigned, output pixels)
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeDofTiles(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	RWTexture2D tiles = gRWTextures[SHADER_TYPES_DOF_TILES_RW_TEXTURE];
	uint2 tileCount;
	tiles.GetDimensions(tileCount.x, tileCount.y);
	uint2 tile = dispatchThreadId.xy;
	if (any(tile >= tileCount))
		return;
	uint2 outputSize;
	gTextures[SHADER_TYPES_LENSED_TEXTURE].GetDimensions(outputSize.x, outputSize.y);
	LensData lens = gLensData[gPushConstants.frameIndex][0];
	// the view at the tile's center (a tile across two views takes one's circles for both: the reach is a bound)
	float2 center = float2(tile * SHADER_TYPES_DOF_TILE_SIZE) + 0.5 * float(SHADER_TYPES_DOF_TILE_SIZE);
	ViewData view;
	float4 viewport;
	float mmPerPixel;
	if (!DofViewAt(min(center, float2(outputSize) - 1.0), outputSize, lens, view, viewport, mmPerPixel))
	{
		tiles[tile] = 0.0;
		return;
	}
	float largest = 0.0;
	for (uint y = 0; y < SHADER_TYPES_DOF_TILE_SIZE; y += 2u)
		for (uint x = 0; x < SHADER_TYPES_DOF_TILE_SIZE; x += 2u)
		{
			float2 pixel = float2(tile * SHADER_TYPES_DOF_TILE_SIZE + uint2(x, y));
			if (any(pixel >= float2(outputSize)))
				continue;
			float3 circles = DofCircles(lens, DofDistanceMm(view, DofDepthAt(pixel, outputSize)), mmPerPixel);
			largest = max(largest, max(abs(circles.x), max(abs(circles.y), abs(circles.z))));
		}
	tiles[tile] = float4(largest, 0.0, 0.0, 0.0);
}

// each tile's largest circle among its neighbors within the largest circle's reach (3 tiles each way)
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeDofTilesDilate(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	RWTexture2D dilated = gRWTextures[SHADER_TYPES_DOF_TILES_DILATED_RW_TEXTURE];
	uint2 tileCount;
	dilated.GetDimensions(tileCount.x, tileCount.y);
	uint2 tile = dispatchThreadId.xy;
	if (any(tile >= tileCount))
		return;
	Texture2D tiles = gTextures[SHADER_TYPES_DOF_TILES_TEXTURE];
	float largest = 0.0;
	for (int y = -3; y <= 3; y++)
		for (int x = -3; x <= 3; x++)
		{
			int2 at = clamp(int2(tile) + int2(x, y), int2(0, 0), int2(tileCount) - 1);
			float there = tiles.Load(int3(at, 0)).r;
			// only as far as that tile's circle reaches
			float reach = there / float(SHADER_TYPES_DOF_TILE_SIZE) + 1.0;
			if (float(max(abs(x), abs(y))) <= reach)
				largest = max(largest, there);
		}
	dilated[tile] = float4(largest, 0.0, 0.0, 0.0);
}

// the frame at half the output's size: the 2x2's mean color, and the circle (the green's, signed, output pixels) of the
// one of the four with the largest, which spreads farthest
[shader("compute")]
[numthreads(16, 16, 1)]
void ComputeDofPrepare(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	RWTexture2D half = gRWTextures[SHADER_TYPES_DOF_HALF_RW_TEXTURE];
	uint2 size;
	half.GetDimensions(size.x, size.y);
	uint2 texel = dispatchThreadId.xy;
	if (any(texel >= size))
		return;
	uint2 outputSize;
	gTextures[SHADER_TYPES_LENSED_TEXTURE].GetDimensions(outputSize.x, outputSize.y);
	LensData lens = gLensData[gPushConstants.frameIndex][0];
	Texture2D frame = gTextures[LensFrameTexture()];
	// the view at the texel's first pixel (the 2x2 share it but at a view's edge)
	ViewData view;
	float4 viewport;
	float mmPerPixel;
	bool inView = DofViewAt(float2(2u * texel), outputSize, lens, view, viewport, mmPerPixel);
	float3 color = 0.0;
	float circle = 0.0;
	[unroll]
	for (uint y = 0; y < 2; y++)
		[unroll]
		for (uint x = 0; x < 2; x++)
		{
			uint2 pixel = min(2u * texel + uint2(x, y), outputSize - 1u);
			color += max(frame.Load(int3(pixel, 0)).rgb, 0.0);
			if (inView)
			{
				float green = DofCircles(lens, DofDistanceMm(view, DofDepthAt(float2(pixel), outputSize)), mmPerPixel).y;
				if (abs(green) > abs(circle))
					circle = green;
			}
		}
	half[texel] = float4(0.25 * color, circle);
}

// the bokeh's shape: a point of the unit disk, shaped by the blades' polygon (a circle without blades), kept where the
// cat's eye leaves it: a disk of the same radius catEye radii toward the center clips it
float2 DofBokehPoint(LensData lens, float2 point)
{
	if (lens.bladeCount < 3u)
		return point;
	float sector = 2.0 * 3.14159265 / float(lens.bladeCount);
	float angle = atan2(point.y, point.x);
	float nearest = round(angle / sector) * sector;
	// the polygon's radius along the point's direction, its circumscribed circle at 1
	float radius = cos(0.5 * sector) / max(cos(angle - nearest), 1e-3);
	return point * radius;
}

// the bokeh's profile at a radius of a point's circle (0 its center, 1 its edge), nearer than the focus or farther
// (see LensData::bokehNear: the spherical aberration's rim or soft edge), as much as the circle is within the reference
// (a larger circle's defocus outgrows the aberration, and the profile tends to uniform)
float DofBokeh(LensData lens, float radius, bool near, float circleMm)
{
	float sample = saturate(radius) * float(SHADER_TYPES_LENS_VIGNETTING_COUNT - 1u);
	uint below = uint(floor(sample));
	uint above = min(below + 1u, SHADER_TYPES_LENS_VIGNETTING_COUNT - 1u);
	float profile = near ? lerp(lens.bokehNear[below / 4u][below % 4u], lens.bokehNear[above / 4u][above % 4u], frac(sample))
						 : lerp(lens.bokehFar[below / 4u][below % 4u], lens.bokehFar[above / 4u][above % 4u], frac(sample));
	float share = lens.bokehReference > 0.0 ? saturate(lens.bokehReference / max(circleMm, 1e-6)) : 0.0;
	return lerp(1.0, profile, share);
}

float DofCatEye(LensData lens, float2 sensor)
{
	float tangent = length(sensor) / max(lens.focalLength, 1e-3);
	float angle = atan(tangent);
	float sample = saturate(angle / max(lens.fieldAngle, 1e-6)) * float(SHADER_TYPES_LENS_VIGNETTING_COUNT - 1u);
	uint below = uint(floor(sample));
	uint above = min(below + 1u, SHADER_TYPES_LENS_VIGNETTING_COUNT - 1u);
	return lerp(lens.catEye[below / 4u][below % 4u], lens.catEye[above / 4u][above % 4u], frac(sample));
}

// the gather at half the output's size: over the tile's reach, kDofTapCount taps on the bokeh's shape (a spiral, turned
// per texel), each counting for a channel where its circle covers the texel (over a texel, antialiased), a tap behind
// the texel's point by as much as the point is sharp (a blurred background doesn't bleed over a sharp foreground), the
// texel itself whole; the share of what counted that lies in front of the point in the alpha (the foreground's
// spread, which the composite keeps over a sharp pixel)
[shader("compute")]
[numthreads(16, 16, 1)]
void ComputeDofGather(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	RWTexture2D blur = gRWTextures[SHADER_TYPES_DOF_BLUR_RW_TEXTURE];
	uint2 size;
	blur.GetDimensions(size.x, size.y);
	uint2 texel = dispatchThreadId.xy;
	if (any(texel >= size))
		return;
	Texture2D half = gTextures[SHADER_TYPES_DOF_HALF_TEXTURE];
	Texture2D tiles = gTextures[SHADER_TYPES_DOF_TILES_DILATED_TEXTURE];
	LensData lens = gLensData[gPushConstants.frameIndex][0];
	float4 center = half.Load(int3(texel, 0));
	// the reach, in half texels (the circles are in output pixels)
	uint2 tile = (2u * texel) / SHADER_TYPES_DOF_TILE_SIZE;
	float reach = 0.5 * tiles.Load(int3(tile, 0)).r;
	if (reach < 0.5)
	{
		blur[texel] = float4(center.rgb, 0.0);
		return;
	}
	// the texel on the sensor, for the cat's eye, pointing to the view's center
	uint2 outputSize;
	gTextures[SHADER_TYPES_LENSED_TEXTURE].GetDimensions(outputSize.x, outputSize.y);
	float2 outputPixel = 2.0 * float2(texel) + 1.0;
	ViewData view;
	float4 viewport;
	float mmPerPixel;
	float2 toCenter = 0.0;
	float catEye = 0.0;
	if (DofViewAt(outputPixel, outputSize, lens, view, viewport, mmPerPixel))
	{
		float2 sensor = (outputPixel - (viewport.xy + 0.5 * viewport.zw)) * LensSensorMmPerPixel(lens, viewport);
		catEye = DofCatEye(lens, sensor);
		toCenter = length(sensor) > 1e-3 ? -normalize(sensor) : float2(0.0, 0.0);
	}
	// the red's and the blue's circles from the green's (see LensData::focusShiftRed: a radius shifted by half the
	// aperture's diameter times the focal length's shift over the focal length), in half texels
	float shiftScale = 0.25 * lens.apertureDiameter / max(lens.focalLength * mmPerPixel, 1e-6);
	float3 circleShift = float3(-lens.focusShiftRed, 0.0, -lens.focusShiftBlue) * shiftScale;
	float3 centerCircle = abs(0.5 * center.a + circleShift);
	float turn = DofNoise(float2(texel)) * 2.0 * 3.14159265;
	float3 sum = center.rgb;
	float3 weight = 1.0;
	float front = 0.0;
	// more taps the farther the reach (a tap a half texel of radius at least), up to twice kDofTapCount
	uint tapCount = clamp(uint(reach * reach * 0.6), kDofTapCount, 2u * kDofTapCount);
	for (uint tapIt = 0; tapIt < tapCount; tapIt++)
	{
		float radius = sqrt((float(tapIt) + 0.5) / float(tapCount));
		float angle = float(tapIt) * kDofGoldenAngle + turn;
		float2 point = DofBokehPoint(lens, radius * float2(cos(angle), sin(angle)));
		// clipped by the cat's eye
		if (length(point - catEye * toCenter) > 1.0)
			continue;
		// an anamorphic lens's bokeh is an oval the squeeze taller than wide: its cylinders shrink the defocus across by
		// the squeeze squared on the sensor (an afocal squeeze of the angles, so of the vergences squared), and the view
		// shows the sensor's width the squeeze wider. the taps on the oval, and how far a tap is in the oval's measure
		// (every circle is the same oval, so a tap covers the texel as a round one would)
		float2 offset = float2(point.x / max(lens.squeeze, 1.0), point.y) * reach;
		int2 at = clamp(int2(floor(float2(texel) + 0.5 + offset)), int2(0, 0), int2(size) - 1);
		float4 tap = half.Load(int3(at, 0));
		float distance = length(point) * reach;
		float3 tapCircle = abs(0.5 * tap.a + circleShift);
		float3 covers = saturate(tapCircle - distance + 1.0);
		// behind the point: as much as the point is blurred itself
		bool behind = tap.a > center.a + 0.5;
		float3 hidden = behind ? saturate(centerCircle / max(tapCircle, 1e-3)) : 1.0;
		// the tap's circle's profile where it covers the texel (see DofBokeh; its radius in mm, two output pixels a half
		// texel)
		float bokeh = DofBokeh(lens, distance / max(tapCircle.y, 1e-3), tap.a < 0.0, 2.0 * tapCircle.y * mmPerPixel);
		float3 tapWeight = covers * hidden * bokeh;
		sum += tap.rgb * tapWeight;
		weight += tapWeight;
		front += behind ? 0.0 : tapWeight.y;
	}
	blur[texel] = float4(sum / weight, saturate(front / weight.y));
}

// the frame at the output's size through the depth of field: the gather's result where the pixel's own circle is past
// a pixel, or the foreground's spread reaches it, else the frame as it is; the letterbox as it is
[shader("compute")]
[numthreads(16, 16, 1)]
void ComputeDofComposite(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	RWTexture2D output = gRWTextures[SHADER_TYPES_DOF_RW_TEXTURE];
	uint2 size;
	output.GetDimensions(size.x, size.y);
	uint2 pixel = dispatchThreadId.xy;
	if (any(pixel >= size))
		return;
	LensData lens = gLensData[gPushConstants.frameIndex][0];
	Texture2D frame = gTextures[LensFrameTexture()];
	float4 sharp = frame.Load(int3(pixel, 0));
	ViewData view;
	float4 viewport;
	float mmPerPixel;
	if (!DofViewAt(float2(pixel), size, lens, view, viewport, mmPerPixel))
	{
		output[pixel] = sharp;
		return;
	}
	float3 circles = DofCircles(lens, DofDistanceMm(view, DofDepthAt(float2(pixel), size)), mmPerPixel);
	float circle = max(abs(circles.x), max(abs(circles.y), abs(circles.z)));
	// the gather's result, a 3x3 tent of it (four bilinear samples half a texel of it apart: the taps' noise at a large
	// reach is dotty)
	float2 uv = (float2(pixel) + 0.5) / float2(size);
	float2 halfTexel = 0.5 / (0.5 * float2(size));
	Texture2D blur = gTextures[SHADER_TYPES_DOF_BLUR_TEXTURE];
	SamplerState clampSampler = gSamplers[SHADER_TYPES_CLAMP_SAMPLER];
	float4 blurred = 0.25 * (blur.SampleLevel(clampSampler, uv + float2(-halfTexel.x, -halfTexel.y), 0.0) + blur.SampleLevel(clampSampler, uv + float2(halfTexel.x, -halfTexel.y), 0.0) +
							 blur.SampleLevel(clampSampler, uv + float2(-halfTexel.x, halfTexel.y), 0.0) + blur.SampleLevel(clampSampler, uv + halfTexel, 0.0));
	float share = max(saturate((circle - 0.5) / 1.5), blurred.a);
	output[pixel] = float4(lerp(max(sharp.rgb, 0.0), blurred.rgb, share), sharp.a);
}
