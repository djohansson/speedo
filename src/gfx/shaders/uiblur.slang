// the ui's frosted panels, as macOS's (the menu bar, menus and popups: see UiBlurRect): the output under each
// (ComputeMain wrote it, and the blur's reach around it, linear, to SHADER_TYPES_UI_BLUR_SOURCE_TEXTURE) blurred by a
// gaussian and a little more saturated, then written back over the output inside the panel's rounded corners, before
// the ui draws its translucent panels over it. a part of shaders.slang, which includes it after capi.h and sky.h

static const int kUiBlurRadius = int(SHADER_TYPES_UI_BLUR_RADIUS);

// a gaussian's weight at offset pixels, whose radius is about 2.5 standard deviations
float UiBlurWeight(int offset)
{
	float sigma = float(kUiBlurRadius) / 2.5;
	return exp(-0.5 * float(offset * offset) / (sigma * sigma));
}

// the blur along one axis in reads of a linear filtered texture (clamped at its edges): the center, then the taps in
// pairs (1 and 2, 3 and 4, ... on either side), each pair one read between them, at the offset weighing the two as the
// gaussian does, with their weight: kUiBlurRadius + 1 reads instead of 2 kUiBlurRadius + 1. step is a pixel along the
// axis, in texture coordinates
float3 UiBlur(Texture2D texture, float2 center, float2 step)
{
	SamplerState clamped = gSamplers[SHADER_TYPES_CLAMP_SAMPLER];
	float weights = UiBlurWeight(0);
	float3 sum = texture.SampleLevel(clamped, center, 0.0).rgb * weights;
	for (int tap = 1; tap <= kUiBlurRadius; tap += 2)
	{
		float near = UiBlurWeight(tap);
		float far = tap + 1 <= kUiBlurRadius ? UiBlurWeight(tap + 1) : 0.0;
		float weight = near + far;
		float offset = float(tap) + far / weight;
		sum += (texture.SampleLevel(clamped, center + step * offset, 0.0).rgb +
				texture.SampleLevel(clamped, center - step * offset, 0.0).rgb) * weight;
		weights += 2.0 * weight;
	}
	return sum / weights;
}

// whether a pixel is within the blur's reach of a panel: where ComputeMain writes the source
bool UiBlurReaches(uint2 pixel)
{
	for (uint rectIt = 0; rectIt < gPushConstants.uiBlurRectCount; rectIt++)
	{
		float4 rect = gUiBlurRects[gPushConstants.frameIndex][rectIt].rect;
		if (all(float2(pixel) >= rect.xy - float(kUiBlurRadius)) && all(float2(pixel) < rect.zw + float(kUiBlurRadius)))
			return true;
	}
	return false;
}

// how much of a pixel the panels cover, antialiased at their rounded corners and edges, times their opacity: the most of
// any, so that overlapping panels' threads write the same
float UiBlurCoverage(int2 pixel)
{
	float2 center = float2(pixel) + 0.5;
	float coverage = 0.0;
	for (uint rectIt = 0; rectIt < gPushConstants.uiBlurRectCount; rectIt++)
	{
		UiBlurRect rect = gUiBlurRects[gPushConstants.frameIndex][rectIt];
		float2 halfSize = (rect.rect.zw - rect.rect.xy) * 0.5;
		float radius = min(rect.radius, min(halfSize.x, halfSize.y));
		float2 q = abs(center - (rect.rect.xy + halfSize)) - (halfSize - radius);
		float distance = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
		coverage = max(coverage, saturate(0.5 - distance) * rect.opacity);
	}
	return coverage;
}

// the pixel a UI Blur thread is for: the threads are the panels' pixels in turn, row by row from the panel's first
// thread (firstThreadX: the X pass's, over each panel and the blur's reach above and below it; firstThreadY: the Y
// pass's, over each panel). false past the last panel's
bool UiBlurPixel(uint thread, bool alongX, out int2 pixel)
{
	pixel = int2(0, 0);
	for (uint rectIt = gPushConstants.uiBlurRectCount; rectIt-- > 0;)
	{
		UiBlurRect rect = gUiBlurRects[gPushConstants.frameIndex][rectIt];
		uint first = alongX ? rect.firstThreadX : rect.firstThreadY;
		if (thread < first)
			continue;
		int reach = alongX ? kUiBlurRadius : 0;
		int2 topLeft = int2(rect.rect.xy) - int2(0, reach);
		int2 bottomRight = int2(rect.rect.zw) + int2(0, reach);
		uint width = uint(max(bottomRight.x - topLeft.x, 1));
		uint local = thread - first;
		pixel = topLeft + int2(int(local % width), int(local / width));
		return pixel.y < bottomRight.y;
	}
	return false;
}

// the UI Blur X pass: the source blurred along x, over each panel and the blur's reach above and below it
[shader("compute")]
[numthreads(SHADER_TYPES_UI_BLUR_GROUP_SIZE, 1, 1)]
void ComputeUiBlurX(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	Texture2D source = gTextures[SHADER_TYPES_UI_BLUR_SOURCE_TEXTURE];
	uint2 size;
	source.GetDimensions(size.x, size.y);
	int2 pixel;
	if (!UiBlurPixel(dispatchThreadId.x, true, pixel) || any(pixel < 0) || any(pixel >= int2(size)))
		return;

	float3 blurred = UiBlur(source, (float2(pixel) + 0.5) / float2(size), float2(1.0 / float(size.x), 0.0));
	gRWTextures[SHADER_TYPES_UI_BLUR_ROWS_RW_TEXTURE].Store(uint2(pixel), float4(blurred, 1.0));
}

// the UI Blur Y pass: those rows blurred along y, over the output inside each panel's rounded rectangle
[shader("compute")]
[numthreads(SHADER_TYPES_UI_BLUR_GROUP_SIZE, 1, 1)]
void ComputeUiBlurY(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	Texture2D rows = gTextures[SHADER_TYPES_UI_BLUR_ROWS_TEXTURE];
	RWTexture2D output = gRWTextures[gPushConstants.frameIndex];
	uint2 size;
	output.GetDimensions(size.x, size.y);
	int2 pixel;
	if (!UiBlurPixel(dispatchThreadId.x, false, pixel) || any(pixel < 0) || any(pixel >= int2(size)))
		return;
	float coverage = UiBlurCoverage(pixel);
	if (coverage <= 0.0)
		return;

	float3 color = UiBlur(rows, (float2(pixel) + 0.5) / float2(size), float2(0.0, 1.0 / float(size.y)));
	// macOS's vibrancy: the colors behind come through a little stronger
	float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
	color = saturate(lerp(float3(luminance), color, 1.3));
	// the panel's antialiased edge over the unblurred output
	float3 under = gTextures[SHADER_TYPES_UI_BLUR_SOURCE_TEXTURE].Load(int3(pixel, 0)).rgb;
	output.Store(uint2(pixel), float4(ApplySRGBCurve(lerp(under, color, coverage)), 1.0));
}
