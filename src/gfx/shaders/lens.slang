// the camera's lens (see LensData, gfx/lens.h): the Lens pass, ComputeLens, makes of the frame at the output's size what
// the lens does to it, between its reconstruction (or its composite, without temporal upscaling) and the final pass:
// the vignetting over the field, the lateral chromatic aberration and the distortion, each from the prescription. then
// the glare: the Bloom pass veils the lens's image over itself (ComputeBloomDown, ComputeBloomUp: a chain of blurs at
// half the output's size and down, summed back into the first level, a share of which the final pass adds), the Flare
// Sources pass finds the bright blobs the frame holds (ComputeFlareSources: the sun's disk, lamps, lit windows), and the
// Flare pass draws their ghosts and starbursts over the lens's image (flare.slang, a layout of its own). a part of
// shaders.slang, which includes it after capi.h and sky.h

// a blob is a source of glare from this many times white at the view's exposure, where its texel is this many times
// its surroundings' mean (compact: the sun's disk, a lamp's bulb, a glint; a sunlit wall or a window by day isn't), and
// a ghost or a starburst is drawn from this share of white (see ComputeFlareSources, VertexFlare)
static const float kFlareSourceLeastLuminance = 64.0;
static const float kFlareSourceLeastContrast = 4.0;
static const float kFlareLeastVisible = 1e-3;
// the sources tracked from frame to frame (see ComputeFlareTrack): a candidate within this many output pixels of a
// source is the same, its place and light blended in by this share, and a source's strength rises or falls by this
// much a frame as it is found again or not
static const float kFlareTrackReach = 32.0;
static const float kFlareTrackBlend = 0.2;
static const float kFlareTrackRamp = 0.15;
// and a source's place stands while the candidate strays within this many pixels of it (the centroid moves with the
// frames' jitter, and a streak a pixel wide with it)
static const float kFlareTrackDeadZone = 1.5;

float LensLuminance(float3 color)
{
	return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// the sensor's millimeters per output pixel of a view, each way: the sensor's half diagonal is the view's corner from
// its center, and an anamorphic lens's image is squeezed across onto the sensor, the view showing it unsqueezed, so a
// pixel across is a squeeze less on the sensor (see Views::SetLens; FlareSensorMmPerPixel in flare.slang is the same)
float2 LensSensorMmPerPixel(LensData lens, float4 viewport)
{
	float2 toSensor = float2(1.0 / max(lens.squeeze, 1.0), 1.0);
	float halfDiagonalPixels = 0.5 * length(viewport.zw * toSensor);
	return toSensor * lens.halfDiagonal / max(halfDiagonalPixels, 1.0);
}

// the vignetting at a point of the sensor (mm from its center): the light it gets as a share of the axis's, by its
// field angle, over the lens's table from the axis to the corner
float LensVignetting(LensData lens, float2 sensor)
{
	float tangent = length(sensor) / max(lens.focalLength, 1e-3);
	float angle = atan(tangent);
	float sample = saturate(angle / max(lens.fieldAngle, 1e-6)) * float(SHADER_TYPES_LENS_VIGNETTING_COUNT - 1u);
	uint below = uint(floor(sample));
	uint above = min(below + 1u, SHADER_TYPES_LENS_VIGNETTING_COUNT - 1u);
	return lerp(lens.vignetting[below / 4u][below % 4u], lens.vignetting[above / 4u][above % 4u], frac(sample));
}

// the lens's image of the frame at a pixel of the output (see ComputeLens), in a view: the pixel's place in the lens's
// field, by its distance from the view's center over the view's half diagonal (the sensor's half diagonal, at the view's
// corner; on the sensor, which an anamorphic lens's image is squeezed across onto, the view shown unsqueezed, so the
// pixel's offset across is the squeeze more on the view than on the sensor); the distortion maps the pixel, on the
// lens's image, to the point of the ideal image the frame rendered (the ideal radius of a distorted one, the third order
// inverted once: r (1 - k r^2)), which the frame holds wider by the margin so that barrel distortion leaves no corner
// empty; the lateral aberration takes the red and the blue from the ideal points at their images' scale; and the
// vignetting at the field angle scales what the three read
float3 LensSample(Texture2D source, SamplerState linear, LensData lens, float4 viewport, uint2 pixel, uint2 size)
{
	float2 viewportOrigin = viewport.xy;
	float2 viewportSize = viewport.zw;
	float2 center = viewportOrigin + 0.5 * viewportSize;
	float2 toSensor = float2(1.0 / max(lens.squeeze, 1.0), 1.0);
	float halfDiagonalPixels = 0.5 * length(viewportSize * toSensor);
	float2 fromCenter = (float2(pixel) + 0.5 - center) * toSensor / halfDiagonalPixels; // the corner at 1
	float distorted = length(fromCenter);
	float ideal = distorted * (1.0 - lens.distortion * distorted * distorted);
	float2 direction = distorted > 1e-6 ? fromCenter / distorted : float2(0.0, 0.0);
	// the source, the frame rendered: its radius over the margin, per channel over its image's scale
	float3 scales = float3(lens.redScale, 1.0, lens.blueScale) * lens.margin;
	float3 color;
	[unroll]
	for (uint channel = 0; channel < 3; channel++)
	{
		float radius = ideal / max(scales[channel], 1e-3);
		float2 at = center + direction * radius * halfDiagonalPixels / toSensor;
		// within the view's viewport (the sampler clamps at the image's edge, not a split view's)
		at = clamp(at, viewportOrigin + 0.5, viewportOrigin + viewportSize - 0.5);
		color[channel] = source.SampleLevel(linear, at / float2(size), 0.0)[channel];
	}
	// the vignetting where the pixel is on the lens's image (the distorted radius, in mm)
	float vignetting = LensVignetting(lens, fromCenter * lens.halfDiagonal);
	return color * (vignetting * lens.transmittance);
}

// the view's viewport in the output's pixels (ViewData's is in the render target's)
float4 LensOutputViewport(ViewData view, uint2 size)
{
	float2 scale = float2(size) / float2(UpscaleRenderSize());
	return float4(view.viewport.xy * scale, view.viewport.zw * scale);
}

// the frame the lens takes, at the output's size: reconstructed (the parity's), or composited at it without temporal
// upscaling (the gTextures slot)
uint LensFrameTexture()
{
	bool upscaled = (gPushConstants.upscale & UPSCALE_SHARPEN) != 0u;
	return upscaled ? SHADER_TYPES_UPSCALE_COLOR_TEXTURE + UpscaleParity() : SHADER_TYPES_COMPOSITE_TEXTURE;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void ComputeLens(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint frameIndex = gPushConstants.frameIndex;
	LensData lens = gLensData[frameIndex][0];
	RWTexture2D lensed = gRWTextures[SHADER_TYPES_LENSED_RW_TEXTURE];
	uint2 size;
	lensed.GetDimensions(size.x, size.y);
	uint2 pixel = dispatchThreadId.xy;
	if (any(pixel >= size))
		return;
	// the frame, through the depth of field and the motion blur where they are drawn (see ComputeDofComposite,
	// ComputeMotionBlur)
	Texture2D source = gTextures[lens.motionBlur != 0u ? SHADER_TYPES_MOTION_BLUR_TEXTURE : lens.dof != 0u ? SHADER_TYPES_DOF_TEXTURE : LensFrameTexture()];
	ViewData view;
	if (!UpscaleViewAtUv((float2(pixel) + 0.5) / float2(size), view))
	{
		lensed[pixel] = source.Load(int3(pixel, 0));
		return;
	}
	float4 viewport = LensOutputViewport(view, size);
	float3 color = LensSample(source, gSamplers[SHADER_TYPES_CLAMP_SAMPLER], lens, viewport, pixel, size);
	lensed[pixel] = float4(color, 1.0);
}

// the bloom's chain, a level down (see SHADER_TYPES_BLOOM_TEXTURE: PushConstants::depthPyramidLevel says which): the
// level above, the lens's image for the first, filtered over 13 taps a texel of it apart (Jimenez 2014: the 2x2 at half
// a texel weighing half, the 3x3 at a texel the rest; its weights sum to 1, so the light is kept: not the partial Karis
// average, which weighs a tap down by its brightness and dropped the sun's disk, thousands of times white, from the
// chain, with the glare it is the source of)
[shader("compute")]
[numthreads(16, 16, 1)]
void ComputeBloomDown(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint level = gPushConstants.depthPyramidLevel;
	RWTexture2D<float4> destination = gBloomLevels[level];
	uint2 size;
	destination.GetDimensions(size.x, size.y);
	uint2 pixel = dispatchThreadId.xy;
	if (any(pixel >= size))
		return;
	float3 result;
	if (level == 0u)
	{
		// the lens's image, sampled: twice this level's size (but for rounding), so the taps are half a texel of this
		// level apart
		float2 uv = (float2(pixel) + 0.5) / float2(size);
		float2 texel = 0.5 / float2(size);
		Texture2D source = gTextures[SHADER_TYPES_LENSED_TEXTURE];
		SamplerState linear = gSamplers[SHADER_TYPES_CLAMP_SAMPLER];
		float3 taps[13];
		const float2 kOffsets[13] = {
			float2(-2.0, -2.0), float2(0.0, -2.0), float2(2.0, -2.0),
			float2(-2.0, 0.0), float2(0.0, 0.0), float2(2.0, 0.0),
			float2(-2.0, 2.0), float2(0.0, 2.0), float2(2.0, 2.0),
			float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0)};
		[unroll]
		for (uint tapIt = 0; tapIt < 13; tapIt++)
			taps[tapIt] = max(source.SampleLevel(linear, uv + kOffsets[tapIt] * texel, 0.0).rgb, 0.0);
		result = taps[4] * 0.125 + (taps[0] + taps[2] + taps[6] + taps[8]) * 0.03125 + (taps[1] + taps[3] + taps[5] + taps[7]) * 0.0625 +
				 (taps[9] + taps[10] + taps[11] + taps[12]) * 0.125;
	}
	else
	{
		// the level above through its storage view (written by the dispatch before, in this pass: a sampled view of
		// the chain read what its memory held before the pass on KosmicKrisp), a 4x4 tent over the 2x2 it holds
		RWTexture2D<float4> source = gBloomLevels[level - 1u];
		uint2 sourceSize;
		source.GetDimensions(sourceSize.x, sourceSize.y);
		int2 base = int2(2u * pixel);
		const float kWeights[4] = {1.0, 3.0, 3.0, 1.0};
		result = 0.0;
		[unroll]
		for (int y = -1; y <= 2; y++)
			[unroll]
			for (int x = -1; x <= 2; x++)
			{
				int2 at = clamp(base + int2(x, y), int2(0, 0), int2(sourceSize) - 1);
				result += kWeights[x + 1] * kWeights[y + 1] * max(source[uint2(at)].rgb, 0.0);
			}
		result /= 64.0;
	}
	destination[pixel] = float4(result, 1.0);
}

// the bloom's chain, a level up: the level below (coarser), filtered over a 3x3 tent a texel of it apart, added to this
// level, so that the first holds every level's blur (SHADER_TYPES_BLOOM_LEVEL_COUNT of them, which the final pass
// divides by)
[shader("compute")]
[numthreads(16, 16, 1)]
void ComputeBloomUp(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint level = gPushConstants.depthPyramidLevel;
	RWTexture2D<float4> destination = gBloomLevels[level];
	uint2 size;
	destination.GetDimensions(size.x, size.y);
	uint2 pixel = dispatchThreadId.xy;
	if (any(pixel >= size))
		return;
	// the level below through its storage view (see ComputeBloomDown), a 3x3 tent around the texel this one is in
	RWTexture2D<float4> source = gBloomLevels[level + 1u];
	uint2 sourceSize;
	source.GetDimensions(sourceSize.x, sourceSize.y);
	int2 base = int2(pixel / 2u);
	float3 sum = 0.0;
	const float kWeights[3] = {1.0, 2.0, 1.0};
	[unroll]
	for (int y = -1; y <= 1; y++)
		[unroll]
		for (int x = -1; x <= 1; x++)
		{
			int2 at = clamp(base + int2(x, y), int2(0, 0), int2(sourceSize) - 1);
			sum += kWeights[x + 1] * kWeights[y + 1] * max(source[uint2(at)].rgb, 0.0);
		}
	destination[pixel] = float4(destination[pixel].rgb + sum / 16.0, 1.0);
}

// the glare's sources (see LensSource, the Flare pass): a thread per texel of the bloom's third level (an eighth of the
// output each way) finds the blobs brighter than kFlareSourceLeastLuminance at the view's exposure whose texel is the
// brightest of their 5x5 (ties by place), and lists each (gLensSources, counted in gLensSourceCount, the first
// SHADER_TYPES_LENS_CANDIDATE_COUNT kept): where it is on the sensor (the luminance weighted centroid of the 5x5's bright
// texels, mm from the view's center) and its light at the lens (the texels' radiance times their solid angle, over the
// vignetting the lens's image was darkened by there), if its brightest ghost or its starburst would show
[shader("compute")]
[numthreads(16, 16, 1)]
void ComputeFlareSources(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint kLevel = 2u;
	RWTexture2D<float4> bloom = gBloomLevels[kLevel];
	uint2 outputSize;
	gTextures[SHADER_TYPES_LENSED_TEXTURE].GetDimensions(outputSize.x, outputSize.y);
	uint2 size;
	bloom.GetDimensions(size.x, size.y);
	uint2 texel = dispatchThreadId.xy;
	if (any(texel >= size))
		return;
	LensData lens = gLensData[gPushConstants.frameIndex][0];
	ViewData view;
	if (!UpscaleViewAtUv((float2(texel) + 0.5) / float2(size), view))
		return;
	float4 viewport = LensOutputViewport(view, outputSize);
	float exposure = view.exposure;
	float3 here = max(bloom[texel].rgb, 0.0);
	float luminance = LensLuminance(here) * exposure;
	if (luminance < kFlareSourceLeastLuminance)
		return;
	float3 fluxSum = 0.0;
	float2 centroid = 0.0;
	float weightSum = 0.0;
	float ringSum = 0.0;
	for (int y = -2; y <= 2; y++)
		for (int x = -2; x <= 2; x++)
		{
			int2 at = clamp(int2(texel) + int2(x, y), int2(0, 0), int2(size) - 1);
			float3 color = max(bloom[uint2(at)].rgb, 0.0);
			float there = LensLuminance(color) * exposure;
			bool before = at.y < int(texel.y) || (at.y == int(texel.y) && at.x < int(texel.x));
			if (any(at != int2(texel)) && (there > luminance || (there == luminance && before)))
				return;
			if (max(abs(x), abs(y)) == 2)
				ringSum += there;
			if (there >= 0.25 * kFlareSourceLeastLuminance)
			{
				fluxSum += color;
				centroid += there * (float2(at) + 0.5);
				weightSum += there;
			}
		}
	// compact: well above the ring around it
	if (luminance < kFlareSourceLeastContrast * ringSum / 16.0)
		return;
	centroid /= max(weightSum, 1e-6);
	// on the sensor, and the light: the texels' radiance times their solid angle (each texelPixels squared pixels of
	// (a pixel's mm over the focal length) squared steradians)
	float2 texelPixels = float2(outputSize) / float2(size);
	float2 mmPerPixel = LensSensorMmPerPixel(lens, viewport);
	float2 sensor = (centroid * texelPixels - (viewport.xy + 0.5 * viewport.zw)) * mmPerPixel;
	float2 texelMm = texelPixels * mmPerPixel;
	float solidAngle = texelMm.x * texelMm.y / max(lens.focalLength * lens.focalLength, 1e-6);
	float3 flux = fluxSum * solidAngle / max(LensVignetting(lens, sensor) * lens.transmittance, 1e-3);
	// worth drawing: its brightest ghost, or its starburst where its far field begins (the near field is every pixel's,
	// see ComputeStarburst: half the reach, in half texels of two pixels each), above kFlareLeastVisible at the exposure
	float farField = float(SHADER_TYPES_STAR_REACH) * min(mmPerPixel.x, mmPerPixel.y);
	float light = LensLuminance(flux) * exposure;
	if (light * max(lens.ghostGain, lens.starGain / (farField * farField)) < kFlareLeastVisible)
		return;
	// and bright enough to be among the brightest the sources keep (half the least of a full list's, last frame, see
	// ComputeFlareTrack), so that a frame of glints doesn't fill the candidates with arbitrary ones
	if (light < 0.5 * asfloat(gLensSourceCount[3]))
		return;
	// a candidate (see ComputeFlareTrack)
	uint index;
	InterlockedAdd(gLensSourceCount[SHADER_TYPES_LENS_SOURCE_CANDIDATES], 1u, index);
	if (index >= SHADER_TYPES_LENS_CANDIDATE_COUNT)
		return;
	LensSource source;
	source.position = float4(sensor, exposure, 0.0);
	source.flux = float4(flux, 0.0);
	source.viewport = viewport;
	gLensSources[SHADER_TYPES_LENS_SOURCE_CANDIDATES * SHADER_TYPES_LENS_SOURCE_COUNT + index] = source;
}

// the frame's sources from the last frame's and the candidates (one thread: at most SHADER_TYPES_LENS_SOURCE_COUNT of
// the last frame's, SHADER_TYPES_LENS_CANDIDATE_COUNT candidates): each of the last frame's matched to the nearest
// candidate of its view within kFlareTrackReach, its place and light blended toward it and its strength up by
// kFlareTrackRamp, or, unmatched, its strength down by as much and dropped at 0; then the candidates left, the brightest
// first, as new sources of that strength, while there is room. so a flare fades in over a few frames and out over a
// few, and never jumps between the blobs a bright frame holds. a full list keeps its least light at the exposure, which
// the next frame's candidates must reach half of (gLensSourceCount's fourth word)
[shader("compute")]
[numthreads(1, 1, 1)]
void ComputeFlareTrack(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	LensData lens = gLensData[gPushConstants.frameIndex][0];
	uint parity = lens.grainFrame & 1u;
	uint previousParity = parity ^ 1u;
	uint previousFirst = previousParity * SHADER_TYPES_LENS_SOURCE_COUNT;
	uint candidateFirst = SHADER_TYPES_LENS_SOURCE_CANDIDATES * SHADER_TYPES_LENS_SOURCE_COUNT;
	uint currentFirst = parity * SHADER_TYPES_LENS_SOURCE_COUNT;
	uint previousCount = min(gLensSourceCount[previousParity], SHADER_TYPES_LENS_SOURCE_COUNT);
	uint candidateCount = min(gLensSourceCount[SHADER_TYPES_LENS_SOURCE_CANDIDATES], SHADER_TYPES_LENS_CANDIDATE_COUNT);
	bool taken[SHADER_TYPES_LENS_CANDIDATE_COUNT];
	for (uint candidateIt = 0; candidateIt < SHADER_TYPES_LENS_CANDIDATE_COUNT; candidateIt++)
		taken[candidateIt] = false;
	uint count = 0;
	for (uint previousIt = 0; previousIt < previousCount; previousIt++)
	{
		LensSource source = gLensSources[previousFirst + previousIt];
		float2 mmPerPixel = LensSensorMmPerPixel(lens, source.viewport);
		float reach = kFlareTrackReach * max(mmPerPixel.x, mmPerPixel.y);
		uint nearest = SHADER_TYPES_LENS_CANDIDATE_COUNT;
		float nearestDistance = reach;
		for (uint candidateIt = 0; candidateIt < candidateCount; candidateIt++)
		{
			if (taken[candidateIt])
				continue;
			LensSource candidate = gLensSources[candidateFirst + candidateIt];
			if (any(candidate.viewport != source.viewport))
				continue;
			float distance = length(candidate.position.xy - source.position.xy);
			if (distance < nearestDistance)
			{
				nearest = candidateIt;
				nearestDistance = distance;
			}
		}
		if (nearest < SHADER_TYPES_LENS_CANDIDATE_COUNT)
		{
			taken[nearest] = true;
			LensSource candidate = gLensSources[candidateFirst + nearest];
			if (nearestDistance > kFlareTrackDeadZone * max(mmPerPixel.x, mmPerPixel.y))
				source.position.xy = lerp(source.position.xy, candidate.position.xy, kFlareTrackBlend);
			source.position.z = candidate.position.z;
			source.position.w = min(source.position.w + kFlareTrackRamp, 1.0);
			source.flux.rgb = lerp(source.flux.rgb, candidate.flux.rgb, kFlareTrackBlend);
		}
		else
		{
			source.position.w -= kFlareTrackRamp;
			if (source.position.w <= 0.0)
				continue;
		}
		if (count < SHADER_TYPES_LENS_SOURCE_COUNT)
			gLensSources[currentFirst + count++] = source;
	}
	// the candidates left, the brightest first
	while (count < SHADER_TYPES_LENS_SOURCE_COUNT)
	{
		uint brightest = SHADER_TYPES_LENS_CANDIDATE_COUNT;
		float brightestLight = 0.0;
		for (uint candidateIt = 0; candidateIt < candidateCount; candidateIt++)
		{
			if (taken[candidateIt])
				continue;
			LensSource candidate = gLensSources[candidateFirst + candidateIt];
			float light = LensLuminance(candidate.flux.rgb) * candidate.position.z;
			if (brightest == SHADER_TYPES_LENS_CANDIDATE_COUNT || light > brightestLight)
			{
				brightest = candidateIt;
				brightestLight = light;
			}
		}
		if (brightest == SHADER_TYPES_LENS_CANDIDATE_COUNT)
			break;
		taken[brightest] = true;
		LensSource source = gLensSources[candidateFirst + brightest];
		source.position.w = kFlareTrackRamp;
		gLensSources[currentFirst + count++] = source;
	}
	gLensSourceCount[parity] = count;
	// a full list's least light, for the next frame's candidates (none while there is room)
	float least = 0.0;
	if (count == SHADER_TYPES_LENS_SOURCE_COUNT)
	{
		least = 3.0e38;
		for (uint sourceIt = 0; sourceIt < count; sourceIt++)
		{
			LensSource source = gLensSources[currentFirst + sourceIt];
			least = min(least, LensLuminance(source.flux.rgb) * source.position.z);
		}
	}
	gLensSourceCount[3] = asuint(least);
}

// each source's size (after ComputeFlareTrack, a group of 16x16 threads a source of the frame's list): the disk as large
// as the lens's image's pixels above half the brightest, within kFlareSizeWindow output pixels each way around it (the
// sun's disk is uniform, a lamp's bulb near it), its radius on the sensor (mm, in LensSource::flux.w). the sprites spread
// a spike across by it (see FlareStarburst): an extended source's spikes are bands as wide as it, as the near field
// gathers them (ComputeStarburst), and drawn as lines from a point the sun's turned thin where the far field began
static const uint kFlareSizeWindow = 64u;
groupshared float gFlareSizeShared[256];

[shader("compute")]
[numthreads(16, 16, 1)]
void ComputeFlareSize(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
	LensData lens = gLensData[gPushConstants.frameIndex][0];
	uint parity = lens.grainFrame & 1u;
	uint sourceIndex = groupId.x;
	if (sourceIndex >= min(gLensSourceCount[parity], SHADER_TYPES_LENS_SOURCE_COUNT))
		return;
	uint index = parity * SHADER_TYPES_LENS_SOURCE_COUNT + sourceIndex;
	LensSource source = gLensSources[index];
	Texture2D lensed = gTextures[SHADER_TYPES_LENSED_TEXTURE];
	uint2 outputSize;
	lensed.GetDimensions(outputSize.x, outputSize.y);
	float2 mmPerPixel = LensSensorMmPerPixel(lens, source.viewport);
	float2 center = source.viewport.xy + 0.5 * source.viewport.zw + source.position.xy / mmPerPixel;
	int2 first = int2(floor(center)) - int(kFlareSizeWindow / 2u);
	// each thread a 4x4 of the window: the brightest, then how many are above half of it
	const uint kCell = kFlareSizeWindow / 16u;
	float values[kCell * kCell];
	float brightest = 0.0;
	for (uint y = 0; y < kCell; y++)
		for (uint x = 0; x < kCell; x++)
		{
			int2 at = first + int2(groupThreadId.xy * kCell + uint2(x, y));
			bool inside = all(at >= int2(source.viewport.xy)) && all(at < int2(source.viewport.xy + source.viewport.zw)) && all(at < int2(outputSize));
			float value = inside ? LensLuminance(max(lensed.Load(int3(at, 0)).rgb, 0.0)) : 0.0;
			values[y * kCell + x] = value;
			brightest = max(brightest, value);
		}
	uint thread = groupThreadId.y * 16u + groupThreadId.x;
	gFlareSizeShared[thread] = brightest;
	GroupMemoryBarrierWithGroupSync();
	for (uint stride = 128u; stride > 0u; stride /= 2u)
	{
		if (thread < stride)
			gFlareSizeShared[thread] = max(gFlareSizeShared[thread], gFlareSizeShared[thread + stride]);
		GroupMemoryBarrierWithGroupSync();
	}
	float halfPeak = 0.5 * gFlareSizeShared[0];
	GroupMemoryBarrierWithGroupSync();
	float count = 0.0;
	for (uint cellIt = 0; cellIt < kCell * kCell; cellIt++)
		count += values[cellIt] >= halfPeak && halfPeak > 0.0 ? 1.0 : 0.0;
	gFlareSizeShared[thread] = count;
	GroupMemoryBarrierWithGroupSync();
	for (uint stride = 128u; stride > 0u; stride /= 2u)
	{
		if (thread < stride)
			gFlareSizeShared[thread] += gFlareSizeShared[thread + stride];
		GroupMemoryBarrierWithGroupSync();
	}
	if (thread == 0u)
	{
		// the disk of that area, on the sensor (an anamorphic lens's pixels are narrower there across: the area's)
		float radiusPixels = sqrt(gFlareSizeShared[0] / 3.14159265);
		gLensSources[index].flux.w = radiusPixels * sqrt(mmPerPixel.x * mmPerPixel.y);
	}
}

// the starburst's near field (the Starburst pass, see ComputeStarburst): each tile's brightest, SHADER_TYPES_STAR_TILE_SIZE
// half texels each way (twice as many output pixels), the largest channel at the view's exposure: a group per tile, a
// thread per half texel, its 2x2 of the lens's image
groupshared float gStarTileBrightest[SHADER_TYPES_STAR_TILE_SIZE * SHADER_TYPES_STAR_TILE_SIZE];

[shader("compute")]
[numthreads(SHADER_TYPES_STAR_TILE_SIZE, SHADER_TYPES_STAR_TILE_SIZE, 1)]
void ComputeStarTiles(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
	Texture2D lensed = gTextures[SHADER_TYPES_LENSED_TEXTURE];
	uint2 outputSize;
	lensed.GetDimensions(outputSize.x, outputSize.y);
	uint2 pixel = 2u * dispatchThreadId.xy;
	float brightest = 0.0;
	float3 sum = 0.0;
	ViewData view;
	if (all(pixel < outputSize) && UpscaleViewAtUv((float2(pixel) + 1.0) / float2(outputSize), view))
	{
		[unroll]
		for (uint y = 0; y < 2; y++)
			[unroll]
			for (uint x = 0; x < 2; x++)
			{
				uint2 at = min(pixel + uint2(x, y), outputSize - 1u);
				float3 color = max(lensed.Load(int3(at, 0)).rgb, 0.0);
				sum += color;
				brightest = max(brightest, max(color.r, max(color.g, color.b)) * view.exposure);
			}
	}
	// and the image the gather takes, at half the output's size (each half texel its 2x2's mean, black outside the views):
	// sampling the lens's image at the output's size cost twice as much at the reach (6.5 ms more at 1440p, 8 blades)
	RWTexture2D source = gRWTextures[SHADER_TYPES_STAR_SOURCE_RW_TEXTURE];
	uint2 sourceSize;
	source.GetDimensions(sourceSize.x, sourceSize.y);
	if (all(dispatchThreadId.xy < sourceSize))
		source[dispatchThreadId.xy] = float4(0.25 * sum, 1.0);
	uint thread = groupThreadId.y * SHADER_TYPES_STAR_TILE_SIZE + groupThreadId.x;
	gStarTileBrightest[thread] = brightest;
	GroupMemoryBarrierWithGroupSync();
	for (uint stride = SHADER_TYPES_STAR_TILE_SIZE * SHADER_TYPES_STAR_TILE_SIZE / 2u; stride > 0u; stride /= 2u)
	{
		if (thread < stride)
			gStarTileBrightest[thread] = max(gStarTileBrightest[thread], gStarTileBrightest[thread + stride]);
		GroupMemoryBarrierWithGroupSync();
	}
	if (thread == 0u)
	{
		RWTexture2D tiles = gRWTextures[SHADER_TYPES_STAR_TILES_RW_TEXTURE];
		uint2 tileCount;
		tiles.GetDimensions(tileCount.x, tileCount.y);
		if (all(groupId.xy < tileCount))
			tiles[groupId.xy] = float4(min(gStarTileBrightest[0], 65000.0), 0.0, 0.0, 0.0);
	}
}

// the blades' polygon's edges' straight length (mm; FlareEdgeLength in flare.slang, lens::StraightEdgeLength): the
// stop's radius its apothem, its edge 2 a tan(pi / N), or the chord the round opening leaves of it (none wide open)
float StarEdgeLength(LensData lens)
{
	if (lens.bladeCount < 3u)
		return 0.0;
	float open = lens.openRadius > 0.0 ? lens.openRadius : 1.0e30;
	float edge = 2.0 * lens.stopRadius * tan(3.14159265 / float(lens.bladeCount));
	float chord = 2.0 * sqrt(max(open * open - lens.stopRadius * lens.stopRadius, 0.0));
	return min(edge, chord);
}

// a straight edge's far field over the image, per channel (mm): the edge's length times the f-number squared times the
// wavelength over pi cubed times the focal length (see FlareStarburst: the streak of a source of light F at a distance r
// along the edge's normal holds F times the edge's length squared times the f-number squared over pi cubed, times the
// wavelength times the focal length over the edge, over r squared, across it; spread over every point of the image, a
// pixel takes from the points along each direction their radiance times that over the focal length squared, times the
// step over r squared: this over the step, over the step's count squared)
float3 StarCoefficient(LensData lens, float edge)
{
	float fNumber = lens.focalLength / max(2.0 * lens.pupilRadius, 1e-3);
	const float3 kWavelengths = float3(650.0e-6, 550.0e-6, 450.0e-6); // mm (lens::kWavelengths)
	return edge * fNumber * fNumber * kWavelengths / (31.0062767 * max(lens.focalLength, 1e-3));
}

// the starburst's near field (the aperture's blades' diffraction, and an anamorphic lens's cylinder's streak across, see
// FlareStarburst), of every point of the image: a thread per half texel gathers along each of the spikes' directions (the
// blades' edges' normals either way: an even count's opposite edges share a line, twice as bright; an odd count's 2N
// spikes) the lens's image a half texel a step, each step's radiance times StarCoefficient over the step on the sensor
// over the step's count squared, as far as the brightest tile around would show (kFlareLeastVisible at the exposure) and
// at most SHADER_TYPES_STAR_REACH steps, fading out over the second half, where the sources' sprites take over (the far
// field, see VertexFlare). so a frame of glints, each a pixel, has a star on each, whatever their number; the final pass
// adds it
[shader("compute")]
[numthreads(16, 16, 1)]
void ComputeStarburst(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	RWTexture2D star = gRWTextures[SHADER_TYPES_STAR_RW_TEXTURE];
	uint2 size;
	star.GetDimensions(size.x, size.y);
	uint2 texel = dispatchThreadId.xy;
	if (any(texel >= size))
		return;
	Texture2D lensed = gTextures[SHADER_TYPES_LENSED_TEXTURE];
	uint2 outputSize;
	lensed.GetDimensions(outputSize.x, outputSize.y);
	LensData lens = gLensData[gPushConstants.frameIndex][0];
	// the half texel's center, the corner of its 2x2 of output pixels (a linear tap there is their mean)
	float2 pixel = 2.0 * float2(texel) + 1.0;
	ViewData view;
	float edge = StarEdgeLength(lens);
	bool cylinder = lens.squeeze > 1.0;
	if (!UpscaleViewAtUv(pixel / float2(outputSize), view) || (edge <= 0.0 && !cylinder))
	{
		star[texel] = float4(0.0, 0.0, 0.0, 0.0);
		return;
	}
	float4 viewport = LensOutputViewport(view, outputSize);
	float2 mmPerPixel = LensSensorMmPerPixel(lens, viewport);
	bool evenBlades = (lens.bladeCount & 1u) == 0u;
	float3 bladeCoefficient = edge > 0.0 ? StarCoefficient(lens, edge) * (evenBlades ? 2.0 : 1.0) : 0.0;
	// the cylinder's edge, the lens's whole clear aperture, tinted blue as its coatings reflect it (FlareStarburst's)
	float3 cylinderCoefficient = cylinder ? StarCoefficient(lens, 2.0 * lens.entranceRadius) * float3(0.55, 0.75, 1.0) : 0.0;
	// the reach: the brightest tile within it, its spikes to kFlareLeastVisible at the shortest step
	Texture2D tiles = gTextures[SHADER_TYPES_STAR_TILES_TEXTURE];
	uint2 tileCount;
	tiles.GetDimensions(tileCount.x, tileCount.y);
	int2 tile = int2(texel / SHADER_TYPES_STAR_TILE_SIZE);
	const int kTileReach = int(SHADER_TYPES_STAR_REACH / SHADER_TYPES_STAR_TILE_SIZE);
	float brightest = 0.0;
	for (int y = -kTileReach; y <= kTileReach; y++)
		for (int x = -kTileReach; x <= kTileReach; x++)
			brightest = max(brightest, tiles.Load(int3(clamp(tile + int2(x, y), int2(0, 0), int2(tileCount) - 1), 0)).r);
	float strongest = max(max(bladeCoefficient.r, cylinderCoefficient.r), max(max(bladeCoefficient.g, cylinderCoefficient.g), max(bladeCoefficient.b, cylinderCoefficient.b)));
	float shortestStep = 2.0 * min(mmPerPixel.x, mmPerPixel.y);
	float reach = min(float(SHADER_TYPES_STAR_REACH), sqrt(brightest * strongest / (shortestStep * kFlareLeastVisible)));
	if (reach < 1.0)
	{
		star[texel] = float4(0.0, 0.0, 0.0, 0.0);
		return;
	}
	uint stepCount = uint(ceil(reach));
	uint bladeDirectionCount = edge > 0.0 ? (evenBlades ? lens.bladeCount : 2u * lens.bladeCount) : 0u;
	uint directionCount = bladeDirectionCount + (cylinder ? 2u : 0u);
	float sector = bladeDirectionCount > 0u ? 2.0 * 3.14159265 / float(bladeDirectionCount) : 0.0;
	// the lens's image at half the output's size (see ComputeStarTiles), in half texels, within the view's viewport
	Texture2D source = gTextures[SHADER_TYPES_STAR_SOURCE_TEXTURE];
	SamplerState linear = gSamplers[SHADER_TYPES_CLAMP_SAMPLER];
	float2 center = float2(texel) + 0.5;
	float2 low = 0.5 * viewport.xy + 0.5;
	float2 high = 0.5 * (viewport.xy + viewport.zw) - 0.5;
	float3 sum = 0.0;
	for (uint directionIt = 0; directionIt < directionCount; directionIt++)
	{
		bool blade = directionIt < bladeDirectionCount;
		// the spike's direction on the sensor (an edge's normal, or across), on the view (an anamorphic lens's image is
		// squeezed across onto the sensor), a half texel a step there, and the step on the sensor
		float2 normal = blade ? float2(cos(float(directionIt) * sector), sin(float(directionIt) * sector)) : float2(directionIt == bladeDirectionCount ? 1.0 : -1.0, 0.0);
		float2 onView = normalize(normal / mmPerPixel);
		float stepMm = length(2.0 * onView * mmPerPixel);
		float3 coefficient = (blade ? bladeCoefficient : cylinderCoefficient) / stepMm;
		float3 along = 0.0;
		for (uint stepIt = 1u; stepIt <= stepCount; stepIt++)
		{
			float2 at = center - float(stepIt) * onView;
			if (any(at < low) || any(at > high))
				break;
			float distance = float(stepIt);
			float fade = 1.0 - smoothstep(0.5 * float(SHADER_TYPES_STAR_REACH), float(SHADER_TYPES_STAR_REACH), distance);
			along += max(source.SampleLevel(linear, at / float2(size), 0.0).rgb, 0.0) * (fade / (distance * distance));
		}
		sum += along * coefficient;
	}
	star[texel] = float4(sum, 1.0);
}
