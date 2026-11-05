// the camera's lens (see LensData, gfx/lens.h): the Lens pass, ComputeLens, makes of the frame at the output's size what
// the lens does to it, between its reconstruction (or its composite, without temporal upscaling) and the final pass:
// the vignetting over the field, the lateral chromatic aberration and the distortion, each from the prescription. then
// the glare: the Bloom pass veils the lens's image over itself (ComputeBloomDown, ComputeBloomUp: a chain of blurs at
// half the output's size and down, summed back into the first level, a share of which the final pass adds), the Flare
// Sources pass finds the bright blobs the frame holds (ComputeFlareSources: the sun's disk, lamps, lit windows), and the
// Flare pass draws their ghosts and starbursts over the lens's image (flare.slang, a layout of its own). a part of
// shaders.slang, which includes it after capi.h and sky.h

// a blob is a source of glare from this many times white at the view's exposure, and a ghost or a starburst is drawn
// from this share of white (see ComputeFlareSources, VertexFlare)
static const float kFlareSourceLeastLuminance = 8.0;
static const float kFlareLeastVisible = 1e-3;

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
	// the frame: reconstructed at the output's size (the parity's), or composited at it without temporal upscaling
	bool upscaled = (gPushConstants.upscale & UPSCALE_SHARPEN) != 0u;
	Texture2D source = gTextures[upscaled ? SHADER_TYPES_UPSCALE_COLOR_TEXTURE + UpscaleParity() : SHADER_TYPES_COMPOSITE_TEXTURE];
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
// SHADER_TYPES_LENS_SOURCE_COUNT kept): where it is on the sensor (the luminance weighted centroid of the 5x5's bright
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
	for (int y = -2; y <= 2; y++)
		for (int x = -2; x <= 2; x++)
		{
			int2 at = clamp(int2(texel) + int2(x, y), int2(0, 0), int2(size) - 1);
			float3 color = max(bloom[uint2(at)].rgb, 0.0);
			float there = LensLuminance(color) * exposure;
			bool before = at.y < int(texel.y) || (at.y == int(texel.y) && at.x < int(texel.x));
			if (any(at != int2(texel)) && (there > luminance || (there == luminance && before)))
				return;
			if (there >= 0.25 * kFlareSourceLeastLuminance)
			{
				fluxSum += color;
				centroid += there * (float2(at) + 0.5);
				weightSum += there;
			}
		}
	centroid /= max(weightSum, 1e-6);
	// on the sensor, and the light: the texels' radiance times their solid angle (each texelPixels squared pixels of
	// (a pixel's mm over the focal length) squared steradians)
	float2 texelPixels = float2(outputSize) / float2(size);
	float2 mmPerPixel = LensSensorMmPerPixel(lens, viewport);
	float2 sensor = (centroid * texelPixels - (viewport.xy + 0.5 * viewport.zw)) * mmPerPixel;
	float2 texelMm = texelPixels * mmPerPixel;
	float solidAngle = texelMm.x * texelMm.y / max(lens.focalLength * lens.focalLength, 1e-6);
	float3 flux = fluxSum * solidAngle / max(LensVignetting(lens, sensor) * lens.transmittance, 1e-3);
	// worth drawing: its brightest ghost, or its starburst at 5 mm, above kFlareLeastVisible at the exposure
	float gain = max(lens.ghostGain, lens.starGain / 25.0);
	if (LensLuminance(flux) * exposure * gain < kFlareLeastVisible)
		return;
	uint index;
	InterlockedAdd(gLensSourceCount[0], 1u, index);
	if (index >= SHADER_TYPES_LENS_SOURCE_COUNT)
		return;
	LensSource source;
	source.position = float4(sensor, exposure, 0.0);
	source.flux = float4(flux, 0.0);
	source.viewport = viewport;
	gLensSources[index] = source;
}
