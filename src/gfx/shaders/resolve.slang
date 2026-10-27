// ComputeResolve: temporal upscaling, the frame's composite at the render size resolved at the output's with its history;
// ComputeSharpen: that into the output, tonemapped and sharpened.
// a part of shaders.slang, which includes it after capi.h and sky.h
//
// the frame is rendered with its projection offset within a pixel, a point of a sequence a frame (see Views::Upload), so
// that its pixels see other points each frame. each output pixel takes the composite's 3x3 render pixels around where it
// is, each by how near the point it saw (its center less the jitter) is to the output pixel's center (a gaussian of
// about half a render pixel, Blackman-Harris's as Karis fits it), and its history: the last frame's resolve where the
// point the nearest of the 3x3 sees was (reprojected by its depth through the last frame's view projection, the jitters
// taken out; what moves by its motion, see FragmentPrepass, since its depth is where it is now), bicubic, clipped to
// the box of the 3x3's colors (their mean, and a deviation and a quarter around it: light the history holds that isn't
// this frame's), and blended in by the weight of the samples each holds (at most
// kResolveHistoryWeight: at the output's size about a tenth of the frame's a frame; where the render pixels are fewer
// than the output's, less of their weight lands on each output pixel, which takes more frames). blended as a
// tonemapper's curve compresses the light (Karis), so that a bright sample doesn't flicker as the jitter moves.

static const float kResolveHistoryWeight = 16.0;
static const float kResolveClipDeviations = 1.25;

float ResolveLuminance(float3 color)
{
	return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// the light compressed for blending, and back
float3 ResolveCompress(float3 color)
{
	return color / (1.0 + ResolveLuminance(color));
}

float3 ResolveExpand(float3 color)
{
	return color / max(1.0 - ResolveLuminance(color), 1e-4);
}

float3 ResolveToYCoCg(float3 color)
{
	return float3(dot(color, float3(0.25, 0.5, 0.25)), dot(color, float3(0.5, 0.0, -0.5)), dot(color, float3(-0.25, 0.5, -0.25)));
}

float3 ResolveFromYCoCg(float3 color)
{
	return float3(color.x + color.y - color.z, color.x + color.z, color.x - color.y - color.z);
}

// color moved toward center until it is inside the box of half sizes extent around it
float3 ResolveClip(float3 color, float3 center, float3 extent)
{
	float3 offset = color - center;
	float3 units = abs(offset) / max(extent, 1e-5);
	float largest = max(units.x, max(units.y, units.z));
	return largest > 1.0 ? center + offset / largest : color;
}

// a texture at uv by Catmull-Rom's spline over its 4x4 texels, in 5 bilinear reads (the corners' left out, their weight
// over the rest's)
float4 ResolveSampleBicubic(Texture2D source, SamplerState linear, float2 uv, float2 size)
{
	float2 position = uv * size;
	float2 first = floor(position - 0.5) + 0.5;
	float2 f = position - first;
	float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	float2 w3 = f * f * (-0.5 + 0.5 * f);
	float2 w12 = w1 + w2;
	float2 at0 = (first - 1.0) / size;
	float2 at3 = (first + 2.0) / size;
	float2 at12 = (first + w2 / w12) / size;
	float4 result = source.SampleLevel(linear, float2(at12.x, at0.y), 0.0) * (w12.x * w0.y);
	result += source.SampleLevel(linear, float2(at0.x, at12.y), 0.0) * (w0.x * w12.y);
	result += source.SampleLevel(linear, at12, 0.0) * (w12.x * w12.y);
	result += source.SampleLevel(linear, float2(at3.x, at12.y), 0.0) * (w3.x * w12.y);
	result += source.SampleLevel(linear, float2(at12.x, at3.y), 0.0) * (w12.x * w3.y);
	float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
	return result / weight;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void ComputeResolve(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint parity = (gPushConstants.upscale & UPSCALE_PARITY) != 0u ? 1u : 0u;
	RWTexture2D resolved = gRWTextures[SHADER_TYPES_RESOLVE_HISTORY_RW_TEXTURE + parity];
	uint2 outputSize;
	resolved.GetDimensions(outputSize.x, outputSize.y);
	uint2 xy = dispatchThreadId.xy;
	if (any(xy >= outputSize))
		return;
	int2 renderSize = int2(gPushConstants.framebufferWidth, gPushConstants.framebufferHeight);

	// where the output pixel's center is in the render target's pixels, and the view it is in; and the frame's jitter
	// there, in its pixels
	float2 position = (float2(xy) + 0.5) * float2(renderSize) / float2(outputSize);
	ViewData view;
	bool inView = ViewAt(int2(position), view);
	float2 jitter = inView ? view.jitter.xy * 0.5 * view.viewport.zw : 0.0;
	// a debug view of values (see DebugViewIsData) as it is
	bool data = DebugViewIsData(gPushConstants.debugView);
	float exposure = data ? 1.0 : gPushConstants.exposure;

	// the 3x3 render pixels around it: their light by their points' distance from its center, the box of their colors,
	// and the nearest of them, and whether it moves
	Texture2D composite = gTextures[SHADER_TYPES_COMPOSITE_TEXTURE];
	Texture2D depths = gTextures[SHADER_TYPES_MAIN_DEPTH_TEXTURE];
	int2 center = int2(floor(position + jitter));
	float3 sum = 0.0;
	float sampleWeight = 0.0;
	float3 mean = 0.0;
	float3 square = 0.0;
	int2 nearest = clamp(center, int2(0), renderSize - 1);
	float nearestDepth = 2.0;
	bool nearestMoves = false;
	for (int y = -1; y <= 1; y++)
	{
		for (int x = -1; x <= 1; x++)
		{
			int2 pixel = clamp(center + int2(x, y), int2(0), renderSize - 1);
			float3 compressed = ResolveCompress(composite.Load(int3(pixel, 0)).rgb * exposure);
			float3 ycocg = ResolveToYCoCg(compressed);
			mean += ycocg;
			square += ycocg * ycocg;
			float2 offset = float2(pixel) + 0.5 - jitter - position;
			float weight = exp(-2.29 * dot(offset, offset));
			sum += compressed * weight;
			sampleWeight += weight;
			float depth = depths.Load(int3(pixel, 0)).r;
			if (depth < nearestDepth)
			{
				nearestDepth = depth;
				nearest = pixel;
				nearestMoves = (gMainStencil.Load(int3(pixel, 0)) & 1u) != 0u;
			}
		}
	}
	float3 current = sum / max(sampleWeight, 1e-6);
	mean /= 9.0;
	float3 deviation = sqrt(max(square / 9.0 - mean * mean, 0.0));

	// the history: the last frame's resolve where the nearest pixel's point was, clipped to the box
	float3 history = current;
	float historyWeight = 0.0;
	if ((gPushConstants.upscale & UPSCALE_HISTORY) != 0u && inView && view.previousEyePosition.w != 0.0)
	{
		float2 nearestNdc = (float2(nearest) + 0.5 - view.viewport.xy) / view.viewport.zw * 2.0 - 1.0;
		float4 world = mul(view.inverseViewProjection, float4(nearestNdc, nearestDepth, 1.0));
		world = world.w < 0.0 ? -world : world; // at w 0 a direction (the sky, where the far plane is infinite)
		float4 previousClip = mul(view.previousViewProjection, world);
		if (previousClip.w > 0.0)
		{
			float2 motion = (previousClip.xy / previousClip.w - view.jitter.zw) - (nearestNdc - view.jitter.xy);
			float2 ndc = (position - view.viewport.xy) / view.viewport.zw * 2.0 - 1.0 + motion;
			float2 previous = (ndc * 0.5 + 0.5) * view.viewport.zw + view.viewport.xy;
			// what moves by its own motion (its depth reprojects where it is now, not where it was)
			if (nearestMoves)
				previous = position + gTextures[SHADER_TYPES_PREPASS_MOTION_TEXTURE].Load(int3(nearest, 0)).xy;
			if (all(previous >= view.viewport.xy) && all(previous < view.viewport.xy + view.viewport.zw))
			{
				float4 sampled = ResolveSampleBicubic(
					gTextures[SHADER_TYPES_RESOLVE_HISTORY_TEXTURE + (parity ^ 1u)],
					gSamplers[SHADER_TYPES_CLAMP_SAMPLER],
					previous / float2(renderSize),
					float2(outputSize));
				float3 ycocg = ResolveToYCoCg(ResolveCompress(max(sampled.rgb, 0.0) * exposure));
				history = ResolveFromYCoCg(ResolveClip(ycocg, mean, deviation * kResolveClipDeviations));
				historyWeight = clamp(sampled.a, 0.0, kResolveHistoryWeight);
			}
		}
	}
	float weight = historyWeight + sampleWeight;
	float3 light = ResolveExpand((history * historyWeight + current * sampleWeight) / max(weight, 1e-6)) / exposure;
	resolved.Store(xy, float4(light, min(weight, kResolveHistoryWeight)));

}

// the resolve sharpened into the output (the Sharpen pass): Lottes' robust contrast adaptive sharpening (FSR's RCAS), on
// the tonemapped light: each pixel moved away from its 4 neighbours by the most that takes no channel past 0 or 1 (of
// what they span), at most a fifth of its difference from their mean, less by kSharpenStops. the resolve's blend of
// jittered samples softens what it resolves by about the reconstruction filter's width; this takes that back
static const float kSharpenStops = 0.5;

// the light as the output shows it, before the srgb curve
float3 SharpenMapped(float3 color, bool data)
{
	return saturate(data ? max(color, 0.0) : Tonemap(max(color, 0.0) * gPushConstants.exposure));
}

[shader("compute")]
[numthreads(16, 16, 1)]
void ComputeSharpen(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	RWTexture2D output = gRWTextures[gPushConstants.outputIndex];
	uint2 size;
	output.GetDimensions(size.x, size.y);
	int2 xy = int2(dispatchThreadId.xy);
	if (any(xy >= int2(size)))
		return;
	uint parity = (gPushConstants.upscale & UPSCALE_PARITY) != 0u ? 1u : 0u;
	Texture2D resolved = gTextures[SHADER_TYPES_RESOLVE_HISTORY_TEXTURE + parity];
	bool data = DebugViewIsData(gPushConstants.debugView);
	int2 last = int2(size) - 1;
	float3 e = SharpenMapped(resolved.Load(int3(xy, 0)).rgb, data);
	float3 b = SharpenMapped(resolved.Load(int3(clamp(xy + int2(0, -1), int2(0), last), 0)).rgb, data);
	float3 d = SharpenMapped(resolved.Load(int3(clamp(xy + int2(-1, 0), int2(0), last), 0)).rgb, data);
	float3 f = SharpenMapped(resolved.Load(int3(clamp(xy + int2(1, 0), int2(0), last), 0)).rgb, data);
	float3 h = SharpenMapped(resolved.Load(int3(clamp(xy + int2(0, 1), int2(0), last), 0)).rgb, data);
	float3 lowest = min(min(b, d), min(f, h));
	float3 highest = max(max(b, d), max(f, h));
	float3 toZero = lowest / max(4.0 * highest, 1e-5);
	float3 toOne = (1.0 - highest) / min(4.0 * lowest - 4.0, -1e-5);
	float3 lobes = max(-toZero, toOne);
	float lobe = max(-0.1875, min(max(lobes.r, max(lobes.g, lobes.b)), 0.0)) * exp2(-kSharpenStops);
	float3 mapped = saturate((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0));
	output.Store(uint2(xy), float4(ApplySRGBCurve(mapped), 1.0));
	// the ui's frosted panels' backdrop: under them and within the blur's reach, linear, for the UI Blur passes
	if (gPushConstants.uiBlurRectCount != 0u && UiBlurReaches(uint2(xy)))
		gRWTextures[SHADER_TYPES_UI_BLUR_SOURCE_RW_TEXTURE].Store(uint2(xy), float4(mapped, 1.0));
}
