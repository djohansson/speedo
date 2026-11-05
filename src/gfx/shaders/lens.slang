// the camera's lens (see LensData, gfx/lens.h): the Lens pass, ComputeLens, makes of the frame at the output's size what
// the lens does to it, between its reconstruction (or its composite, without temporal upscaling) and the final pass:
// the vignetting over the field, the lateral chromatic aberration and the distortion, each from the prescription; the
// glare (the veiling bloom, the ghosts, the starburst) follows. a part of shaders.slang, which includes it after capi.h
// and sky.h

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
	// the vignetting at the pixel's field angle: on the lens's image, the distorted radius is where the pixel is
	float tangent = distorted * lens.halfDiagonal / max(lens.focalLength, 1e-3);
	float angle = atan(tangent);
	float sample = saturate(angle / max(lens.fieldAngle, 1e-6)) * float(SHADER_TYPES_LENS_VIGNETTING_COUNT - 1u);
	uint below = uint(floor(sample));
	uint above = min(below + 1u, SHADER_TYPES_LENS_VIGNETTING_COUNT - 1u);
	float vignetting = lerp(lens.vignetting[below / 4u][below % 4u], lens.vignetting[above / 4u][above % 4u], frac(sample));
	return color * (vignetting * lens.transmittance);
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
	// the view's viewport in the output's pixels (ViewData's is in the render target's)
	float2 scale = float2(size) / float2(UpscaleRenderSize());
	float4 viewport = float4(view.viewport.xy * scale, view.viewport.zw * scale);
	float3 color = LensSample(source, gSamplers[SHADER_TYPES_CLAMP_SAMPLER], lens, viewport, pixel, size);
	lensed[pixel] = float4(color, 1.0);
}
