// temporal upscaling: AMD's FidelityFX Super Resolution 3.1 upscaler (the FidelityFX SDK 1.1.4's fsr3upscaler), ported:
// the frame's composite at the render size reconstructed at the output's with the history it keeps. its passes, here
// ComputeUpscalePrepareInputs (ffx_fsr3upscaler_prepare_inputs.h), ComputeUpscaleShadingChangePyramid and
// ComputeUpscaleShadingChange (shading_change_pyramid.h and shading_change.h), ComputeUpscalePrepareReactivity
// (prepare_reactivity.h), ComputeUpscaleLumaInstability (luma_instability.h) and ComputeUpscaleAccumulate (accumulate.h
// with reproject.h and upsample.h), and ComputeSharpen its RCAS. a part of shaders.slang, which includes it after capi.h
// and sky.h
//
// FSR is AMD's, under the MIT license:
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
// Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// what is ours: the inputs. the frame's motion is reconstructed here (UpscaleMotion: what moves by the pre-pass's motion,
// the rest by its depth through the views' last view projections), in the render target's uv (uv + motion is where the
// point was), and the views are viewports of one render target (a grid of them), each pixel's jitter and history its
// view's. the color comes before exposure (the composite), with the frame's exposure (PushConstants::exposure, auto
// exposure's and the slider's) as FSR's and a pre-exposure of 1 (so the history is linear before exposure, and FSR's
// DeltaPreExposure is 1); HDR input (FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT); depth inverted, near 1
// (FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH); motion at the
// render size (LOW_RESOLUTION_MOTION_VECTORS); FSR's defaults for its settings (velocity factor 1, reactiveness and
// shading change scale 1, a third of accumulation a frame, a third less on disocclusion); the scene's units its meters;
// the reactive mask the blended layers' coverage (the composite's alpha, see ComputeMain), and no transparency and
// composition mask. FSR's luma pyramid serves its auto exposure, which ours replaces, and the farthest depth's level its
// accumulation reads, made here with the shading change's pyramid, whose three levels one pass reduces in shared memory
// (FSR's single pass downsampler makes six). the reconstructed previous depth is a buffer of float bits (an atomic
// maximum), cleared before the frame's inputs are prepared. what the first frame after a reset reads of the last frame
// (the luma, the accumulation, the luma history) counts as none.

static const float kUpscaleFp16Min = 6.10e-05;
static const float kUpscaleFp16Max = 65504.0;
static const float kUpscaleEpsilon = kUpscaleFp16Min; // FSR3UPSCALER_EPSILON
static const float kUpscaleTonemapEpsilon = kUpscaleFp16Min;
static const float kUpscaleFp32Max = 3.402823466e+38;
static const float kUpscaleFp32Min = 1.175494351e-38;
// FSR 3.1's constants (ffx_fsr3upscaler_common.h) and settings' defaults (ffx_fsr3upscaler.cpp)
static const float kUpscaleReconstructedDepthBilinearWeightThreshold = kUpscaleEpsilon * 10.0;
static const float kUpscaleLanczosWeightScale = 1.0 / 16.0;
static const float kUpscaleAverageLanczosWeightPerFrame = 0.74 * kUpscaleLanczosWeightScale;
static const float kUpscaleLockThreshold = 1.0;
static const float kUpscaleLockMax = 2.0;
static const float kUpscaleVelocityFactor = 1.0;
static const float kUpscaleAccumulationAddedPerFrame = 1.0 / 3.0;
static const float kUpscaleMinDisocclusionAccumulation = -1.0 / 3.0;
// ours: a render pixel's log2 luma, its running mean and variance by this share a frame (see UpscaleGateShadingChange),
// how many of its deviations it may stray before a shading change counts, and the least deviation (log2 units)
static const float kUpscaleLumaStatisticsBlend = 1.0 / 16.0;
static const float kUpscaleLumaSurpriseDeviations = 3.0;
static const float kUpscaleLumaLeastDeviation = 0.05;
// and the deviation from which the history is kept where it swings (none below the first, all past both: see
// ComputeUpscaleLumaInstability)
static const float kUpscaleLumaSwingLeastDeviation = 0.25;
static const float kUpscaleLumaSwingDeviationRange = 0.5;
// and its drift, the running mean of its log2 luma's difference from its mean by this share a frame, and the shares of
// its deviation from which the pixel drifts (not at all below the first, wholly past both: see UpscaleDrifting)
static const float kUpscaleLumaDriftBlend = 1.0 / 8.0;
static const float kUpscaleLumaDriftLeast = 0.4;
static const float kUpscaleLumaDriftRange = 0.4;
// the shading change's samples (SHADING_CHANGE_SET_SIZE), the first of its pyramid's levels it reads, and how many
static const int kUpscaleShadingChangeSetSize = 5;
static const int kUpscaleShadingChangeMipStart = 0;
static const int kUpscaleShadingChangeMipCount = 3;
// the dilated reactive masks' channels
static const uint kUpscaleReactive = 0u;
static const uint kUpscaleDisocclusion = 1u;
static const uint kUpscaleShadingChange = 2u;
static const uint kUpscaleAccumulation = 3u;
// the blended layers' coverage is reactive up to this (FSR's guidance: reactive values near 1 take no history at all)
static const float kUpscaleReactiveMax = 0.9;

// the frame's sizes: the render target's, the output's (the history's), and the shading change's (half the render
// target's, FSR's ShadingChangeRenderSize)
int2 UpscaleRenderSize()
{
	return int2(gPushConstants.framebufferWidth, gPushConstants.framebufferHeight);
}

int2 UpscaleDisplaySize()
{
	uint2 size;
	gRWTextures[SHADER_TYPES_UPSCALE_COLOR_RW_TEXTURE].GetDimensions(size.x, size.y);
	return int2(size);
}

int2 UpscaleShadingChangeRenderSize()
{
	return UpscaleRenderSize() / 2;
}

int2 UpscaleTextureSize(uint slot)
{
	uint2 size;
	gTextures[slot].GetDimensions(size.x, size.y);
	return int2(size);
}

uint UpscaleParity()
{
	return (gPushConstants.upscale & UPSCALE_PARITY) != 0u ? 1u : 0u;
}

// whether the last frame's images hold a frame at this size (FSR's FrameIndex() != 0)
bool UpscaleHasHistory()
{
	return (gPushConstants.upscale & UPSCALE_HISTORY) != 0u;
}

// FSR's Exposure(): the frame's (1 for a debug view of values, shown as they are, see DebugViewIsData)
float UpscaleExposure()
{
	return DebugViewIsData(gPushConstants.debugView) ? 1.0 : gPushConstants.exposure;
}

// the render size over the output's (FSR's DownscaleFactor)
float2 UpscaleDownscaleFactor()
{
	return float2(UpscaleRenderSize()) / float2(UpscaleDisplaySize());
}

// the jitter's points before they repeat, as Views::Upload picks them (FSR's ffxFsr3UpscalerGetJitterPhaseCount)
float UpscaleJitterSequenceLength()
{
	float2 scale = UpscaleDownscaleFactor();
	return clamp(round(8.0 / (scale.x * scale.y)), 8.0, 64.0);
}

// the view's jitter this frame and the last, in the render target's pixels (FSR's Jitter() and PreviousFrameJitter()): a
// pixel's center sees what is at it less this, unjittered
float2 UpscaleJitter(ViewData view)
{
	return view.jitter.xy * 0.5 * view.viewport.zw;
}

float2 UpscalePreviousJitter(ViewData view)
{
	return view.jitter.zw * 0.5 * view.viewport.zw;
}

// the view a point of the render target's uv is in (its pixel's)
bool UpscaleViewAtUv(float2 uv, out ViewData view)
{
	return ViewAt(int2(clamp(uv * float2(UpscaleRenderSize()), 0.0, float2(UpscaleRenderSize()) - 1.0)), view);
}

// FSR's IsUvInside, of the view's viewport
bool UpscaleUvInside(float2 uv, ViewData view)
{
	float2 at = uv * float2(UpscaleRenderSize());
	return all(at >= view.viewport.xy) && all(at <= view.viewport.xy + view.viewport.zw);
}

// the distance along the view's axis of what is at a depth (FSR's GetViewSpaceDepth: for a perspective projection a
// function of the depth alone; in meters, the scene's units), unprojected at the view's center; a depth at an infinite
// far plane is far
float UpscaleViewDepth(ViewData view, float depth)
{
	float4 world = mul(view.inverseViewProjection, float4(view.jitter.xy, depth, 1.0));
	if (abs(world.w) < 1e-12)
		return kUpscaleFp16Max;
	return min(length(world.xyz / world.w - view.eyePosition.xyz), kUpscaleFp16Max);
}

// where the point at a render pixel was the last frame, from its uv (the jitters taken out): what moves by the pre-pass's
// motion (its depth is where it is now), the rest by its depth through its view's last view projection; none where the
// view has no last frame
float2 UpscaleMotion(int2 pixel, float depth)
{
	ViewData view;
	if (!ViewAt(pixel, view) || view.previousEyePosition.w == 0.0)
		return 0.0;
	float2 renderSize = float2(UpscaleRenderSize());
	if ((gMainStencil.Load(int3(pixel, 0)) & 1u) != 0u)
		return gTextures[SHADER_TYPES_PREPASS_MOTION_TEXTURE].Load(int3(pixel, 0)).xy / renderSize;
	float2 ndc = (float2(pixel) + 0.5 - view.viewport.xy) / view.viewport.zw * 2.0 - 1.0;
	float4 world = mul(view.inverseViewProjection, float4(ndc, depth, 1.0));
	world = world.w < 0.0 ? -world : world; // at w 0 a direction (the sky, where the far plane is infinite)
	float4 previousClip = mul(view.previousViewProjection, world);
	if (previousClip.w <= 0.0)
		return 0.0;
	float2 motion = (previousClip.xy / previousClip.w - view.jitter.zw) - (ndc - view.jitter.xy);
	return motion * 0.5 * view.viewport.zw / renderSize;
}

// FSR's Get4KVelocity: the motion in pixels of a 3840x2160 output
float UpscaleVelocity4K(float2 motion)
{
	return length(motion * float2(3840.0, 2160.0));
}

// the composite's light at a render pixel (FSR's LoadInputColor: linear, before exposure), and the reactive mask (the
// blended layers' coverage)
float3 UpscaleInputColor(int2 pixel)
{
	return gTextures[SHADER_TYPES_COMPOSITE_TEXTURE].Load(int3(pixel, 0)).rgb;
}

float UpscaleReactiveMask(int2 pixel)
{
	return min(gTextures[SHADER_TYPES_COMPOSITE_TEXTURE].Load(int3(pixel, 0)).a, kUpscaleReactiveMax);
}

float UpscaleLoadReconstructedPreviousDepth(int2 pixel)
{
	int2 size = UpscaleRenderSize();
	pixel = clamp(pixel, int2(0), size - 1);
	return asfloat(gUpscaleDepth[pixel.y * size.x + pixel.x]);
}

// the colors and helpers of ffx_fsr3upscaler_common.h
float3 UpscaleYCoCgToRgb(float3 yCoCg)
{
	return float3(yCoCg.x + yCoCg.y - yCoCg.z, yCoCg.x + yCoCg.z, yCoCg.x - yCoCg.y - yCoCg.z);
}

float3 UpscaleRgbToYCoCg(float3 rgb)
{
	return float3(0.25 * rgb.r + 0.5 * rgb.g + 0.25 * rgb.b, 0.5 * rgb.r - 0.5 * rgb.b, -0.25 * rgb.r + 0.5 * rgb.g - 0.25 * rgb.b);
}

float UpscaleRgbToLuma(float3 rgb)
{
	return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}

float3 UpscaleTonemap(float3 rgb)
{
	return rgb / (max(max(0.0, rgb.r), max(rgb.g, rgb.b)) + 1.0);
}

float3 UpscaleInverseTonemap(float3 rgb)
{
	return rgb / max(kUpscaleTonemapEpsilon, 1.0 - max(rgb.r, max(rgb.g, rgb.b)));
}

float UpscaleMinDividedByMax(float v0, float v1, float onZero)
{
	float m = max(v0, v1);
	return m != 0.0 ? min(v0, v1) / m : onZero;
}

float UpscaleMinDividedByMax(float v0, float v1)
{
	return UpscaleMinDividedByMax(v0, v1, 0.0);
}

int2 UpscaleClampLoad(int2 pixel, int2 offset, int2 size)
{
	return clamp(pixel + offset, int2(0), size - 1);
}

float2 UpscaleClampUv(float2 uv, int2 textureSize, int2 resourceSize)
{
	float2 sampleLocation = uv * float2(textureSize);
	float2 clampedLocation = max(0.5, min(sampleLocation, float2(textureSize) - 0.5));
	return clampedLocation / float2(resourceSize);
}

bool UpscaleOnScreen(int2 pixel, int2 size)
{
	return all(uint2(pixel) < uint2(size));
}

// the output pixel a render pixel's sample lands in (FSR's ComputeHrPosFromLrPos), by its view's jitter
int2 UpscaleOutputPixel(int2 pixel, ViewData view)
{
	float2 jittered = float2(pixel) + 0.5 - UpscaleJitter(view);
	return int2(floor(jittered / float2(UpscaleRenderSize()) * float2(UpscaleDisplaySize())));
}

// the distance a motion must have before the reconstructed previous depth takes it, in 4K pixels (FSR's
// ReconstructedDepthMvPxThreshold)
float UpscaleReconstructedDepthMotionThreshold(float nearestDepthInMeters)
{
	return lerp(0.25, 0.75, saturate(nearestDepthInMeters / 100.0));
}

struct UpscaleBilinear
{
	int2 basePosition;
	float weights[4];
};

static const int2 kUpscaleBilinearOffsets[4] = {int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1)};

UpscaleBilinear UpscaleBilinearAt(float2 uv, int2 size)
{
	UpscaleBilinear data;
	float2 position = uv * float2(size) - 0.5;
	data.basePosition = int2(floor(position));
	float2 fraction = frac(position);
	data.weights[0] = (1.0 - fraction.x) * (1.0 - fraction.y);
	data.weights[1] = fraction.x * (1.0 - fraction.y);
	data.weights[2] = (1.0 - fraction.x) * fraction.y;
	data.weights[3] = fraction.x * fraction.y;
	return data;
}

// Lanczos 2 (ffx_fsr3upscaler_sample.h), FSR 1's approximation of its square, which both kernels take
float UpscaleLanczos2ApproxSq(float x2)
{
	x2 = min(x2, 4.0);
	float a = (2.0 / 5.0) * x2 - 1.0;
	float b = (1.0 / 4.0) * x2 - 1.0;
	return ((25.0 / 16.0) * a * a - (25.0 / 16.0 - 1.0)) * (b * b);
}

float4 UpscaleLanczos2Row(float4 color0, float4 color1, float4 color2, float4 color3, float t)
{
	float weight0 = UpscaleLanczos2ApproxSq((-1.0 - t) * (-1.0 - t));
	float weight1 = UpscaleLanczos2ApproxSq(t * t);
	float weight2 = UpscaleLanczos2ApproxSq((1.0 - t) * (1.0 - t));
	float weight3 = UpscaleLanczos2ApproxSq((2.0 - t) * (2.0 - t));
	return (weight0 * color0 + weight1 * color1 + weight2 * color2 + weight3 * color3) / (weight0 + weight1 + weight2 + weight3);
}

// a texture at uv by Lanczos 2 over its 4x4 texels (FSR's HistorySample: DeclareCustomTextureSample and
// DeclareCustomFetchBicubicSamples), approximated (FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE 2, where FSR's default
// is the reference, 0): Bistro's frame at performance 37.5 ms with the reference, 37.1 with this, and as steady
float4 UpscaleSampleLanczos2(Texture2D source, float2 uv, int2 size)
{
	float2 position = uv * float2(size) - 0.5;
	position = max(0.0, min(float2(size), position));
	int2 base = int2(floor(position));
	float2 fraction = frac(position);
	float4 rows[4];
	for (int y = 0; y < 4; y++)
	{
		float4 row[4];
		for (int x = 0; x < 4; x++)
			row[x] = source.Load(int3(UpscaleClampLoad(base, int2(x - 1, y - 1), size), 0));
		rows[y] = UpscaleLanczos2Row(row[0], row[1], row[2], row[3], fraction.x);
	}
	return UpscaleLanczos2Row(rows[0], rows[1], rows[2], rows[3], fraction.y);
}

// the inputs prepared (FSR's PrepareInputs), per render pixel: the nearest depth of the 3x3 around it and its motion (the
// dilated depth and motion), the depth pushed to where the point was the last frame (to the 4 pixels a bilinear read
// there would take, the nearest kept), the farthest depth, and the luma (into the frame's of the two: the next frame's
// shading change compares it)
[shader("compute")]
[numthreads(16, 16, 1)]
void ComputeUpscalePrepareInputs(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	int2 renderSize = UpscaleRenderSize();
	int2 pixel = int2(dispatchThreadId.xy);
	if (any(pixel >= renderSize))
		return;
	ViewData view;
	ViewAt(pixel, view);

	// the depth's extents (FindDepthExtents: the farthest as FSR has it, among the samples that were nearest so far)
	static const int2 kOffsets[9] = {
		int2(0, 0), int2(1, 0), int2(0, 1), int2(0, -1), int2(-1, 0), int2(-1, 1), int2(1, 1), int2(-1, -1), int2(1, -1)};
	Texture2D depths = gTextures[SHADER_TYPES_MAIN_DEPTH_TEXTURE];
	float nearestDepth = depths.Load(int3(pixel, 0)).r;
	float farthestDepth = nearestDepth;
	int2 nearest = pixel;
	for (uint sampleIt = 1; sampleIt < 9; sampleIt++)
	{
		int2 at = pixel + kOffsets[sampleIt];
		if (UpscaleOnScreen(at, renderSize))
		{
			float depth = depths.Load(int3(at, 0)).r;
			if (depth > nearestDepth)
			{
				farthestDepth = min(farthestDepth, depth);
				nearest = at;
				nearestDepth = depth;
			}
		}
	}
	float2 motion = UpscaleMotion(nearest, nearestDepth);

	// the depth where the point was (ReconstructPrevDepth)
	float nearestDepthInMeters = min(UpscaleViewDepth(view, nearestDepth), kUpscaleFp16Max);
	float2 pushed = motion * float(UpscaleVelocity4K(motion) > UpscaleReconstructedDepthMotionThreshold(nearestDepthInMeters));
	float2 uv = (float2(pixel) + 0.5) / float2(renderSize);
	UpscaleBilinear bilinear = UpscaleBilinearAt(uv + pushed, renderSize);
	for (uint sampleIt = 0; sampleIt < 4; sampleIt++)
	{
		if (bilinear.weights[sampleIt] > kUpscaleReconstructedDepthBilinearWeightThreshold)
		{
			int2 at = bilinear.basePosition + kUpscaleBilinearOffsets[sampleIt];
			if (UpscaleOnScreen(at, renderSize))
				InterlockedMax(gUpscaleDepth[at.y * renderSize.x + at.x], asuint(nearestDepth));
		}
	}

	gRWTextures[SHADER_TYPES_UPSCALE_DILATED_MOTION_RW_TEXTURE].Store(uint2(pixel), float4(motion, 0.0, 0.0));
	gRWTextures[SHADER_TYPES_UPSCALE_DILATED_DEPTH_RW_TEXTURE].Store(uint2(pixel), float4(nearestDepth, 0.0, 0.0, 0.0));
	float farthestDepthInMeters = min(UpscaleViewDepth(view, farthestDepth), kUpscaleFp16Max);
	gRWTextures[SHADER_TYPES_UPSCALE_FARTHEST_DEPTH_RW_TEXTURE].Store(uint2(pixel), float4(farthestDepthInMeters, 0.0, 0.0, 0.0));
	float luma = UpscaleRgbToLuma(max(UpscaleInputColor(pixel), 0.0));
	gRWTextures[SHADER_TYPES_UPSCALE_LUMA_RW_TEXTURE + UpscaleParity()].Store(uint2(pixel), float4(luma, 0.0, 0.0, 0.0));
}

// the shading change's sets of samples (SampleSet): a pixel's luma and its 4 neighbours', sorted (SortSet: a sorting
// network of 5)
struct UpscaleSampleSet
{
	float samples[kUpscaleShadingChangeSetSize];
};

static const int2 kUpscaleShadingChangeOffsets[kUpscaleShadingChangeSetSize] = {int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};

void UpscaleCompareSwap(inout UpscaleSampleSet set, int i, int j)
{
	float lower = min(set.samples[i], set.samples[j]);
	set.samples[j] = max(set.samples[i], set.samples[j]);
	set.samples[i] = lower;
}

void UpscaleSortSet(inout UpscaleSampleSet set)
{
	UpscaleCompareSwap(set, 0, 3);
	UpscaleCompareSwap(set, 1, 4);
	UpscaleCompareSwap(set, 0, 2);
	UpscaleCompareSwap(set, 1, 3);
	UpscaleCompareSwap(set, 0, 1);
	UpscaleCompareSwap(set, 2, 4);
	UpscaleCompareSwap(set, 1, 2);
	UpscaleCompareSwap(set, 3, 4);
	UpscaleCompareSwap(set, 2, 3);
}

// the smallest relative difference between the two sets' lumas, signed (ComputeMinimumDifference): walked in order, so
// that a pixel whose luma is any of its neighbours' last frame (an edge the jitter moved) hasn't changed
float UpscaleMinimumDifference(UpscaleSampleSet set0, UpscaleSampleSet set1)
{
	float minimumDifference = kUpscaleFp16Max - 1.0;
	int a = 0;
	int b = 0;
	UpscaleSortSet(set0);
	UpscaleSortSet(set1);
	float maximum = min(set0.samples[kUpscaleShadingChangeSetSize - 1], set1.samples[kUpscaleShadingChangeSetSize - 1]);
	if (maximum > kUpscaleFp32Min)
	{
		for (int sampleIt = 0; sampleIt < kUpscaleShadingChangeSetSize && minimumDifference < kUpscaleFp16Max; sampleIt++)
		{
			float difference = set0.samples[a] - set1.samples[b];
			if (abs(difference) > kUpscaleFp16Min)
			{
				difference = sign(difference) * (1.0 - UpscaleMinDividedByMax(set0.samples[a], set1.samples[b]));
				minimumDifference = abs(difference) < abs(minimumDifference) ? difference : minimumDifference;
				a += int(set0.samples[a] < set1.samples[b]);
				b += int(set0.samples[a] >= set1.samples[b]);
			}
			else
				minimumDifference = kUpscaleFp16Max;
		}
	}
	return minimumDifference * float(minimumDifference < kUpscaleFp16Max - 1.0);
}

// how the luma around a render pixel changed since the last frame where its point was (ComputeDiff), exposed
float UpscaleLumaDifference(int2 pixel, float2 uv, float2 motion, ViewData view)
{
	if (!UpscaleHasHistory())
		return 0.0;
	int2 renderSize = UpscaleRenderSize();
	Texture2D lumas = gTextures[SHADER_TYPES_UPSCALE_LUMA_TEXTURE + UpscaleParity()];
	Texture2D previousLumas = gTextures[SHADER_TYPES_UPSCALE_LUMA_TEXTURE + (UpscaleParity() ^ 1u)];
	float exposure = UpscaleExposure();

	UpscaleSampleSet current;
	int2 base = int2(floor((uv + UpscaleJitter(view) / float2(renderSize)) * float2(renderSize)));
	for (int sampleIt = 0; sampleIt < kUpscaleShadingChangeSetSize; sampleIt++)
	{
		int2 at = UpscaleClampLoad(base, kUpscaleShadingChangeOffsets[sampleIt], renderSize);
		current.samples[sampleIt] = max(lumas.Load(int3(at, 0)).r * exposure, kUpscaleEpsilon);
	}

	float2 reprojectedUv = uv + UpscalePreviousJitter(view) / float2(renderSize) + motion;
	if (!UpscaleUvInside(reprojectedUv, view))
		return 0.0;
	UpscaleSampleSet previous;
	int2 previousBase = int2(floor(reprojectedUv * float2(renderSize)));
	for (int sampleIt = 0; sampleIt < kUpscaleShadingChangeSetSize; sampleIt++)
	{
		int2 at = UpscaleClampLoad(previousBase, kUpscaleShadingChangeOffsets[sampleIt], renderSize);
		previous.samples[sampleIt] = max(previousLumas.Load(int3(at, 0)).r * exposure, kUpscaleEpsilon);
	}
	return UpscaleMinimumDifference(current, previous);
}

// the shading change's pyramid (FSR's ComputeShadingChangePyramid, with the single pass downsampler): per texel at half
// the render size the mean of 2x2 pixels' luma difference and of its sign, then of 2x2 of those, and again (a group of 8x8
// threads makes its 8x8, 4x4 and 2x2 texels of the three levels, through shared memory); and the farthest depth's level
// at half the render size, the luma pyramid's (the mean of 2x2 pixels')
groupshared float2 gUpscalePyramid[8][8];

// a store into a level within its bounds (the groups run past them where its size isn't a multiple of theirs)
void UpscaleStoreWithin(uint slot, uint2 at, float4 value)
{
	uint2 size;
	gRWTextures[slot].GetDimensions(size.x, size.y);
	if (all(at < size))
		gRWTextures[slot].Store(at, value);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeUpscaleShadingChangePyramid(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID, uint3 dispatchThreadId : SV_DispatchThreadID)
{
	int2 renderSize = UpscaleRenderSize();
	Texture2D motions = gTextures[SHADER_TYPES_UPSCALE_DILATED_MOTION_TEXTURE];
	Texture2D farthestDepths = gTextures[SHADER_TYPES_UPSCALE_FARTHEST_DEPTH_TEXTURE];
	float2 sum = 0.0;
	float farthestDepthSum = 0.0;
	for (uint pixelIt = 0; pixelIt < 4; pixelIt++)
	{
		int2 pixel = UpscaleClampLoad(int2(dispatchThreadId.xy) * 2 + int2(pixelIt & 1u, pixelIt >> 1u), int2(0), renderSize);
		ViewData view;
		ViewAt(pixel, view);
		float2 uv = (float2(pixel) + 0.5) / float2(renderSize);
		float difference = UpscaleLumaDifference(pixel, uv, motions.Load(int3(pixel, 0)).xy, view);
		sum += float2(difference, difference != 0.0 ? sign(difference) : 0.0);
		farthestDepthSum += farthestDepths.Load(int3(pixel, 0)).r;
	}
	float2 level0 = sum * 0.25;
	UpscaleStoreWithin(SHADER_TYPES_UPSCALE_SHADING_CHANGE_MIP_RW_TEXTURE, dispatchThreadId.xy, float4(level0, 0.0, 0.0));
	UpscaleStoreWithin(SHADER_TYPES_UPSCALE_FARTHEST_DEPTH_MIP_RW_TEXTURE, dispatchThreadId.xy, float4(farthestDepthSum * 0.25, 0.0, 0.0, 0.0));

	// the two levels below, in shared memory: 8x8 to 4x4, then 4x4 to 2x2 (each read before any thread writes over it)
	gUpscalePyramid[groupThreadId.y][groupThreadId.x] = level0;
	GroupMemoryBarrierWithGroupSync();
	float2 level1 = 0.0;
	if (all(groupThreadId.xy < 4u))
	{
		uint2 at = groupThreadId.xy * 2u;
		level1 = (gUpscalePyramid[at.y][at.x] + gUpscalePyramid[at.y][at.x + 1] + gUpscalePyramid[at.y + 1][at.x] +
				  gUpscalePyramid[at.y + 1][at.x + 1]) * 0.25;
		UpscaleStoreWithin(SHADER_TYPES_UPSCALE_SHADING_CHANGE_MIP_RW_TEXTURE + 1, groupId.xy * 4u + groupThreadId.xy, float4(level1, 0.0, 0.0));
	}
	GroupMemoryBarrierWithGroupSync();
	if (all(groupThreadId.xy < 4u))
		gUpscalePyramid[groupThreadId.y][groupThreadId.x] = level1;
	GroupMemoryBarrierWithGroupSync();
	if (all(groupThreadId.xy < 2u))
	{
		uint2 at = groupThreadId.xy * 2u;
		float2 level2 = (gUpscalePyramid[at.y][at.x] + gUpscalePyramid[at.y][at.x + 1] + gUpscalePyramid[at.y + 1][at.x] +
						 gUpscalePyramid[at.y + 1][at.x + 1]) * 0.25;
		UpscaleStoreWithin(SHADER_TYPES_UPSCALE_SHADING_CHANGE_MIP_RW_TEXTURE + 2, groupId.xy * 2u + groupThreadId.xy, float4(level2, 0.0, 0.0));
	}
}

// the shading change at half the render size (ShadingChange): the most of its pyramid's levels' mean difference times
// their sign's agreement (noise, whose signs disagree, isn't a change)
[shader("compute")]
[numthreads(16, 16, 1)]
void ComputeUpscaleShadingChange(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	int2 size = UpscaleShadingChangeRenderSize();
	int2 pixel = int2(dispatchThreadId.xy);
	if (!UpscaleOnScreen(pixel, size))
		return;
	ViewData view;
	ViewAt(clamp(pixel * 2, int2(0), UpscaleRenderSize() - 1), view);
	float2 uv = (float2(pixel) + 0.5) / float2(size) + UpscaleJitter(view) / float2(UpscaleRenderSize());
	float2 mipUv = UpscaleClampUv(uv, size, UpscaleTextureSize(SHADER_TYPES_UPSCALE_SHADING_CHANGE_MIP_TEXTURE));
	float scale = 1.0 + float(kUpscaleShadingChangeMipStart / kUpscaleShadingChangeMipCount);
	float shadingChange = 0.0;
	for (int level = kUpscaleShadingChangeMipStart; level < kUpscaleShadingChangeMipCount; level++)
	{
		float2 sampled =
			gTextures[SHADER_TYPES_UPSCALE_SHADING_CHANGE_MIP_TEXTURE + level].SampleLevel(gSamplers[SHADER_TYPES_CLAMP_SAMPLER], mipUv, 0.0).xy;
		float change = abs(sampled.x * sampled.y);
		if (change > 0.0)
			shadingChange = max(shadingChange, change) * scale;
	}
	gRWTextures[SHADER_TYPES_UPSCALE_SHADING_CHANGE_RW_TEXTURE].Store(uint2(pixel), float4(saturate(shadingChange), 0.0, 0.0, 0.0));
}

// how much of what the pixel sees the last frame didn't (ComputeDisocclusions), by how much nearer the depth pushed to
// where the point was is than the pixel's
float UpscaleDisocclusion(float2 uv, float2 motion, float currentDepthViewSpace, ViewData view)
{
	int2 renderSize = UpscaleRenderSize();
	float nearestDepthInMeters = min(currentDepthViewSpace, kUpscaleFp16Max);
	motion *= float(UpscaleVelocity4K(motion) > UpscaleReconstructedDepthMotionThreshold(nearestDepthInMeters));
	UpscaleBilinear bilinear = UpscaleBilinearAt(uv + motion, renderSize);
	float disocclusion = 0.0;
	float weightSum = 0.0;
	bool potentialDisocclusion = true;
	for (uint sampleIt = 0; sampleIt < 4 && potentialDisocclusion; sampleIt++)
	{
		int2 at = UpscaleClampLoad(bilinear.basePosition, kUpscaleBilinearOffsets[sampleIt], renderSize);
		if (UpscaleOnScreen(at, renderSize))
		{
			float weight = bilinear.weights[sampleIt];
			if (weight > kUpscaleReconstructedDepthBilinearWeightThreshold)
			{
				float previousNearestDepthViewSpace = UpscaleViewDepth(view, UpscaleLoadReconstructedPreviousDepth(at));
				float depthDifference = currentDepthViewSpace - previousNearestDepthViewSpace;
				potentialDisocclusion = potentialDisocclusion && depthDifference > kUpscaleFp32Min;
				if (potentialDisocclusion)
				{
					float halfViewportWidth = length(view.viewport.zw * 0.5);
					float depthThreshold = max(currentDepthViewSpace, previousNearestDepthViewSpace);
					const float kSeparation = 1.37e-05;
					float requiredDepthSeparation = kSeparation * halfViewportWidth * depthThreshold;
					disocclusion += saturate(requiredDepthSeparation / depthDifference) * weight;
					weightSum += weight;
				}
			}
		}
	}
	return potentialDisocclusion && weightSum > 0.0 ? saturate(1.0 - disocclusion / weightSum) : 0.0;
}

// how little the motion where the point was agrees with its own (ComputeMotionDivergence: the motion field's confidence)
float UpscaleMotionDivergence(float2 uv, float2 motion, float currentDepthSample, ViewData view)
{
	int2 renderSize = UpscaleRenderSize();
	float velocity = UpscaleVelocity4K(motion);
	if (velocity <= 0.0)
		return 0.0;
	int2 reprojected = clamp(int2((uv + motion) * float2(renderSize)), int2(0), renderSize - 1);
	float reprojectedDepth = gTextures[SHADER_TYPES_UPSCALE_DILATED_DEPTH_TEXTURE].Load(int3(reprojected, 0)).r;
	float2 reprojectedMotion = gTextures[SHADER_TYPES_UPSCALE_DILATED_MOTION_TEXTURE].Load(int3(reprojected, 0)).xy;
	float reprojectedVelocity = UpscaleVelocity4K(reprojectedMotion);
	float nucleusDepthInMeters = UpscaleViewDepth(view, reprojectedDepth);
	float currentDepthInMeters = UpscaleViewDepth(view, currentDepthSample);
	float distanceFactor = UpscaleMinDividedByMax(nucleusDepthInMeters, currentDepthInMeters);
	float velocityFactor = saturate(velocity / 10.0);
	return (1.0 - saturate(reprojectedVelocity / velocity)) * distanceFactor * velocityFactor;
}

// the most of the reactive mask around a pixel (DilateReactiveMasks)
float UpscaleDilatedReactiveMask(int2 pixel)
{
	float dilated = 0.0;
	for (int y = -1; y <= 1; y++)
		for (int x = -1; x <= 1; x++)
			dilated = max(dilated, UpscaleReactiveMask(UpscaleClampLoad(pixel, int2(x, y), UpscaleRenderSize())));
	return dilated;
}

// how sure a pixel is a thin feature (ComputeThinFeatureConfidence): a ridge of luma (above or below all of its 3x3's that
// aren't like it) without a 2x2 of alike pixels around it, by how much the 3x3's lumas differ
float UpscaleThinFeatureConfidence(int2 pixel)
{
	// 1 2 3 / 4 0 5 / 6 7 8
	static const int2 kOffsets[9] = {
		int2(0, 0), int2(-1, -1), int2(0, -1), int2(1, -1), int2(-1, 0), int2(1, 0), int2(-1, 1), int2(0, 1), int2(1, 1)};
	static const uint kRejectionMasks[4] = {
		(1u << 1u) | (1u << 2u) | (1u << 4u) | 1u, // upper left
		(1u << 2u) | (1u << 3u) | (1u << 5u) | 1u, // upper right
		(1u << 4u) | (1u << 6u) | (1u << 7u) | 1u, // lower left
		(1u << 5u) | (1u << 7u) | (1u << 8u) | 1u, // lower right
	};
	int2 renderSize = UpscaleRenderSize();
	Texture2D lumas = gTextures[SHADER_TYPES_UPSCALE_LUMA_TEXTURE + UpscaleParity()];
	float samples[9];
	float lumaMinimum = kUpscaleFp32Max;
	float lumaMaximum = kUpscaleFp32Min;
	for (int sampleIt = 0; sampleIt < 9; sampleIt++)
	{
		samples[sampleIt] = lumas.Load(int3(UpscaleClampLoad(pixel, kOffsets[sampleIt], renderSize), 0)).r * UpscaleExposure();
		lumaMinimum = min(lumaMinimum, samples[sampleIt]);
		lumaMaximum = max(lumaMaximum, samples[sampleIt]);
	}
	const float kThreshold = 0.9;
	float dissimilarLumaMinimum = kUpscaleFp32Max;
	float dissimilarLumaMaximum = 0.0;
	uint patternMask = 1u; // the nucleus alike
	for (int sampleIt = 1; sampleIt < 9; sampleIt++)
	{
		float difference = abs(samples[sampleIt] - samples[0]) / (lumaMaximum - lumaMinimum);
		if (difference < kThreshold)
			patternMask |= 1u << uint(sampleIt);
		else
		{
			dissimilarLumaMinimum = min(dissimilarLumaMinimum, samples[sampleIt]);
			dissimilarLumaMaximum = max(dissimilarLumaMaximum, samples[sampleIt]);
		}
	}
	bool ridge = samples[0] > dissimilarLumaMaximum || samples[0] < dissimilarLumaMinimum;
	if (!ridge)
		return 0.0;
	for (uint maskIt = 0; maskIt < 4; maskIt++)
		if ((patternMask & kRejectionMasks[maskIt]) == kRejectionMasks[maskIt])
			return 0.0;
	return lumaMaximum > 0.0 ? 1.0 - lumaMinimum / lumaMaximum : 0.0;
}

// how much the pixel has accumulated (UpdateAccumulation): the last frame's where the point was, none after a shading
// change, a third less after a disocclusion; a third more for the next frame (into the frame's of the two), with the
// luma's statistics (see UpscaleGateShadingChange)
float UpscaleUpdateAccumulation(int2 pixel, float disocclusion, float shadingChange, float4 last, float3 statistics)
{
	float accumulation = saturate(last.r);
	accumulation = lerp(accumulation, 0.0, shadingChange);
	accumulation = lerp(accumulation, min(kUpscaleMinDisocclusionAccumulation, accumulation), disocclusion);
	accumulation *= float(round(accumulation * 100.0) > 1.0);
	gRWTextures[SHADER_TYPES_UPSCALE_ACCUMULATION_RW_TEXTURE + UpscaleParity()].Store(
		uint2(pixel), float4(saturate(accumulation + kUpscaleAccumulationAddedPerFrame), statistics));
	return accumulation;
}

// the last frame's accumulation where the point was (r; 0 without), and its luma's statistics (g, b, a)
float4 UpscaleLastAccumulation(float2 uv, float2 motion, ViewData view, out bool valid)
{
	float2 reprojectedUv = uv + motion;
	valid = UpscaleHasHistory() && UpscaleUvInside(reprojectedUv, view);
	if (!valid)
		return 0.0;
	int2 renderSize = UpscaleRenderSize();
	return gTextures[SHADER_TYPES_UPSCALE_ACCUMULATION_TEXTURE + (UpscaleParity() ^ 1u)].SampleLevel(
		gSamplers[SHADER_TYPES_CLAMP_SAMPLER], UpscaleClampUv(reprojectedUv, renderSize, renderSize), 0.0);
}

// ours, not FSR's: the shading change (the pyramid's, before the reactive mask joins it) only as far as the pixel's
// luma strays from what it usually is, its log2 beyond kUpscaleLumaSurpriseDeviations deviations of its running mean
// (by kUpscaleLumaStatisticsBlend a frame, kept with the accumulation where the point was: statistics, the frame's
// taken in, for the next frame; started over without a history or where revealed). a feature thinner than a render
// pixel is hit by some of the jitter's samples and missed by others, a vertical one whole: its luma swings between
// what it has shown before, which FSR's shading change took for the light changing every frame, and which took its
// locks and its accumulation, so that it showed each frame's samples as they were (AnisotropyBarnLamp's filament
// through its bulb, the user's pose 0.1543,-0.1653,0.3783,-5.337,-31.820 at balanced); a change of the light takes the
// luma past what it was
// how much a render pixel's luma drifts rather than swings (0 to 1), by its statistics (see UpscaleGateShadingChange):
// its running mean of differences from its mean against its deviation. a swing (a thin feature the jitter hits or misses)
// comes back, and its differences cancel; a moving shadow or object pushes the luma one way for frames, and they add
float UpscaleDrifting(float drift, float variance)
{
	return saturate((abs(drift) / max(sqrt(max(variance, 0.0)), kUpscaleLumaLeastDeviation) - kUpscaleLumaDriftLeast) / kUpscaleLumaDriftRange);
}

float UpscaleGateShadingChange(int2 pixel, float shadingChange, float disocclusion, float4 last, bool valid, out float3 statistics)
{
	float luma = log2(max(gTextures[SHADER_TYPES_UPSCALE_LUMA_TEXTURE + UpscaleParity()].Load(int3(pixel, 0)).r, 1e-6));
	if (!valid || disocclusion > 0.5)
	{
		statistics = float3(luma, 0.0, 0.0);
		return shadingChange;
	}
	float mean = last.g;
	float variance = max(last.b, 0.0);
	float difference = luma - mean;
	float allowed = kUpscaleLumaSurpriseDeviations * max(sqrt(variance), kUpscaleLumaLeastDeviation);
	float gate = saturate((abs(difference) - allowed) / allowed);
	// a change past what the pixel allows moves its mean there at once, and its variance by at most what it allows
	// (about half again a frame): a swing that keeps coming back grows the variance in about ten frames, but a change of
	// the light, a step, barely does. taking the whole step into the variance, as it first did, a model appearing over
	// the backdrop (DamagedHelmet installed) was then within its pixels' deviations, its shading change gated and its
	// history kept: it faded in over the backdrop for half a second
	float clamped = min(abs(difference), allowed);
	statistics = float3(gate > 0.0 ? luma : mean + kUpscaleLumaStatisticsBlend * difference,
		(1.0 - kUpscaleLumaStatisticsBlend) * (variance + kUpscaleLumaStatisticsBlend * clamped * clamped),
		lerp(last.a, difference, kUpscaleLumaDriftBlend));
	// and none where the luma drifts (UpscaleDrifting): FSR's shading change, as it was
	return shadingChange * max(gate, UpscaleDrifting(statistics.z, statistics.y));
}

// the reactivity prepared (PrepareReactivity), per render pixel: the dilated reactive masks (the reactiveness, the
// disocclusion, the shading change with the reactive mask, the accumulation), and a new lock where the pixel's sample
// lands for a thin feature
[shader("compute")]
[numthreads(16, 16, 1)]
void ComputeUpscalePrepareReactivity(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	int2 renderSize = UpscaleRenderSize();
	int2 pixel = int2(dispatchThreadId.xy);
	if (any(pixel >= renderSize))
		return;
	ViewData view;
	ViewAt(pixel, view);
	float2 uv = (float2(pixel) + 0.5) / float2(renderSize);
	float2 motion = gTextures[SHADER_TYPES_UPSCALE_DILATED_MOTION_TEXTURE].Load(int3(pixel, 0)).xy;
	float dilatedDepth = gTextures[SHADER_TYPES_UPSCALE_DILATED_DEPTH_TEXTURE].Load(int3(pixel, 0)).r;

	float disocclusion = UpscaleDisocclusion(uv, motion, UpscaleViewDepth(view, dilatedDepth), view);
	// the shading change where the pixel's sample is (ComputeShadingChange: the jitter applied again, taken out when the
	// accumulation samples these masks)
	int2 shadingChangeSize = UpscaleShadingChangeRenderSize();
	float2 shadingChangeUv = UpscaleClampUv(
		uv - UpscaleJitter(view) / float2(renderSize), shadingChangeSize, UpscaleTextureSize(SHADER_TYPES_UPSCALE_SHADING_CHANGE_TEXTURE));
	bool valid;
	float4 last = UpscaleLastAccumulation(uv, motion, view, valid);
	float3 statistics;
	float shadingChange = max(
		UpscaleDilatedReactiveMask(pixel),
		UpscaleGateShadingChange(
			pixel,
			saturate(gTextures[SHADER_TYPES_UPSCALE_SHADING_CHANGE_TEXTURE].SampleLevel(gSamplers[SHADER_TYPES_CLAMP_SAMPLER], shadingChangeUv, 0.0).r),
			disocclusion,
			last,
			valid,
			statistics));
	float reactiveness = UpscaleMotionDivergence(uv, motion, dilatedDepth, view);
	float accumulation = UpscaleUpdateAccumulation(pixel, disocclusion, shadingChange, last, statistics);
	// unorm in FSR
	gRWTextures[SHADER_TYPES_UPSCALE_REACTIVE_RW_TEXTURE].Store(uint2(pixel), saturate(float4(reactiveness, disocclusion, shadingChange, accumulation)));

	float lockStrength = UpscaleThinFeatureConfidence(pixel);
	if (lockStrength > 1.0 / 100.0)
	{
		int2 outputPixel = UpscaleOutputPixel(pixel, view);
		if (UpscaleOnScreen(outputPixel, UpscaleDisplaySize()))
			gRWTextures[SHADER_TYPES_UPSCALE_NEW_LOCKS_RW_TEXTURE].Store(uint2(outputPixel), float4(saturate(lockStrength), 0.0, 0.0, 0.0));
	}
}

// whether the luma flickers between frames (ComputeLumaInstabilityFactor): a luma unlike the last frame's but more like
// one of the three before (by the history of 4 lumas it keeps, unexposed, into the frame's of the two), where the pixel
// has accumulated
[shader("compute")]
[numthreads(16, 16, 1)]
void ComputeUpscaleLumaInstability(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	int2 renderSize = UpscaleRenderSize();
	int2 pixel = int2(dispatchThreadId.xy);
	if (any(pixel >= renderSize))
		return;
	ViewData view;
	ViewAt(pixel, view);
	float4 lumaHistory = 0.0;
	float lumaInstabilityFactor = 0.0;
	float2 motion = gTextures[SHADER_TYPES_UPSCALE_DILATED_MOTION_TEXTURE].Load(int3(pixel, 0)).xy;
	float2 uv = (float2(pixel) + 0.5) / float2(renderSize);
	float2 uvJittered = uv + UpscaleJitter(view) / float2(renderSize);
	float2 reprojectedUv = uv + UpscalePreviousJitter(view) / float2(renderSize) + motion;
	SamplerState linearClamp = gSamplers[SHADER_TYPES_CLAMP_SAMPLER];
	if (UpscaleHasHistory() && UpscaleUvInside(reprojectedUv, view))
	{
		float2 uvSampler = UpscaleClampUv(uvJittered, renderSize, renderSize);
		float4 masks = saturate(gTextures[SHADER_TYPES_UPSCALE_REACTIVE_TEXTURE].SampleLevel(linearClamp, uvSampler, 0.0));
		if (masks[kUpscaleAccumulation] > 0.9)
		{
			float exposure = UpscaleExposure();
			float currentFrameLuma = gTextures[SHADER_TYPES_UPSCALE_LUMA_TEXTURE + UpscaleParity()].SampleLevel(linearClamp, uvSampler, 0.0).r * exposure;
			lumaHistory = gTextures[SHADER_TYPES_UPSCALE_LUMA_HISTORY_TEXTURE + (UpscaleParity() ^ 1u)]
							  .SampleLevel(linearClamp, UpscaleClampUv(reprojectedUv, renderSize, renderSize), 0.0) *
						  exposure;

			float lumaInstability = 0.0;
			float differences0 = currentFrameLuma - lumaHistory[0];
			float similarity0 = UpscaleMinDividedByMax(currentFrameLuma, lumaHistory[0], 1.0);
			float maximumSimilarity = similarity0;
			if (similarity0 < 1.0)
			{
				for (int historyIt = 1; historyIt <= 3; historyIt++)
				{
					float differences1 = currentFrameLuma - lumaHistory[historyIt];
					float similarity1 = UpscaleMinDividedByMax(currentFrameLuma, lumaHistory[historyIt]);
					if (sign(differences0) == sign(differences1))
						maximumSimilarity = max(maximumSimilarity, similarity1);
				}
				lumaInstability = float(maximumSimilarity > similarity0);
			}
			// the history shifted
			lumaHistory = float4(currentFrameLuma, lumaHistory.xyz) / exposure;
			lumaInstabilityFactor = lumaInstability * float(lumaHistory[3] != 0.0);
			// ours: and as much as the pixel's luma swings over frames (its log2's deviation, see UpscaleGateShadingChange):
			// FSR's looks back 4 frames, which the jitter's sequence of up to 64 points outlasts, so that a feature thinner than
			// a render pixel that some of its samples miss had its history clamped onto the frames that missed it, and came and
			// went (AnisotropyBarnLamp's filament; FSR's locks, which keep a thin feature's history, are made at the output
			// pixel a sample lands on, another as the jitter moves, and reached what keeps it at none of them); but not where
			// it drifts (UpscaleDrifting): a shadow or an object moving keeps the luma moving one way, and its history, kept,
			// trailed it and faded out over frames (the user's pose over Bistro's square at 13:21 at 600 times real time:
			// bright bands along every moving shadow's edge)
			float4 statistics = gTextures[SHADER_TYPES_UPSCALE_ACCUMULATION_TEXTURE + UpscaleParity()].Load(int3(pixel, 0));
			float deviation = sqrt(max(statistics.b, 0.0));
			lumaInstabilityFactor = max(lumaInstabilityFactor,
				saturate((deviation - kUpscaleLumaSwingLeastDeviation) / kUpscaleLumaSwingDeviationRange) * (1.0 - UpscaleDrifting(statistics.a, statistics.b)));
			float velocityWeight = 1.0 - saturate(UpscaleVelocity4K(motion) / 20.0);
			lumaInstabilityFactor *= velocityWeight * (1.0 - masks[kUpscaleDisocclusion]) * (1.0 - masks[kUpscaleReactive]) *
									 (1.0 - masks[kUpscaleShadingChange]);
		}
	}
	gRWTextures[SHADER_TYPES_UPSCALE_LUMA_HISTORY_RW_TEXTURE + UpscaleParity()].Store(uint2(pixel), lumaHistory);
	gRWTextures[SHADER_TYPES_UPSCALE_LUMA_INSTABILITY_RW_TEXTURE].Store(uint2(pixel), float4(lumaInstabilityFactor, 0.0, 0.0, 0.0));
}

// what the accumulation's steps share (FSR's AccumulationPassCommonParams)
struct UpscaleParams
{
	int2 outputPixel;
	float2 outputUv;
	float2 renderUvJittered; // the render target's uv the output pixel's center is seen at, jittered (fLrUvJittered)
	float2 renderUvSampler; // and clamped for bilinear reads (fLrUv_HwSampler)
	float2 motion;
	float2 reprojectedUv;
	float velocity4K;
	float disocclusion;
	float reactiveMask;
	float shadingChange;
	float accumulation;
	float lumaInstabilityFactor;
	float farthestDepthInMeters;
	bool existingSample;
	bool newSample;
	ViewData view;
};

// FSR's RectificationBox: the 3x3's colors (their weighted mean and deviation, and their bounds)
struct UpscaleBox
{
	float3 center;
	float3 vec;
	float3 aabbMinimum;
	float3 aabbMaximum;
	float centerWeight;
};

// and what the steps change (AccumulationPassData)
struct UpscaleData
{
	UpscaleBox box;
	float3 upsampledColor;
	float upsampledWeight;
	float3 historyColor;
	float historyWeight;
	float lock;
	float lockContributionThisFrame;
};

void UpscaleBoxAddSample(bool initialSample, inout UpscaleBox box, float3 colorSample, float sampleWeight)
{
	if (initialSample)
	{
		box.aabbMinimum = colorSample;
		box.aabbMaximum = colorSample;
		float3 weightedSample = colorSample * sampleWeight;
		box.center = weightedSample;
		box.vec = colorSample * weightedSample;
		box.centerWeight = sampleWeight;
	}
	else
	{
		box.aabbMinimum = min(box.aabbMinimum, colorSample);
		box.aabbMaximum = max(box.aabbMaximum, colorSample);
		float3 weightedSample = colorSample * sampleWeight;
		box.center += weightedSample;
		box.vec += colorSample * weightedSample;
		box.centerWeight += sampleWeight;
	}
}

void UpscaleBoxComputeVariance(inout UpscaleBox box)
{
	box.centerWeight = abs(box.centerWeight) > kUpscaleFp32Min ? box.centerWeight : 1.0;
	box.center /= box.centerWeight;
	box.vec /= box.centerWeight;
	box.vec = sqrt(abs(box.vec - box.center * box.center));
}

UpscaleParams UpscaleInitParams(int2 outputPixel)
{
	UpscaleParams params;
	int2 renderSize = UpscaleRenderSize();
	params.outputPixel = outputPixel;
	params.outputUv = (float2(outputPixel) + 0.5) / float2(UpscaleDisplaySize());
	UpscaleViewAtUv(params.outputUv, params.view);
	params.renderUvJittered = params.outputUv + UpscaleJitter(params.view) / float2(renderSize);
	params.renderUvSampler = UpscaleClampUv(params.renderUvJittered, renderSize, renderSize);
	// the motion at the render size (LOW_RESOLUTION_MOTION_VECTORS: the dilated motion where the output pixel is)
	params.motion = gTextures[SHADER_TYPES_UPSCALE_DILATED_MOTION_TEXTURE]
						.Load(int3(clamp(int2(params.outputUv * float2(renderSize)), int2(0), renderSize - 1), 0))
						.xy;
	params.velocity4K = UpscaleVelocity4K(params.motion);
	params.reprojectedUv = params.outputUv + params.motion;
	params.existingSample = UpscaleUvInside(params.reprojectedUv, params.view);
	SamplerState linearClamp = gSamplers[SHADER_TYPES_CLAMP_SAMPLER];
	params.lumaInstabilityFactor =
		gTextures[SHADER_TYPES_UPSCALE_LUMA_INSTABILITY_TEXTURE].SampleLevel(linearClamp, UpscaleClampUv(params.outputUv, renderSize, renderSize), 0.0).r;
	float2 farthestDepthUv = UpscaleClampUv(params.renderUvJittered, renderSize / 2, UpscaleTextureSize(SHADER_TYPES_UPSCALE_FARTHEST_DEPTH_MIP_TEXTURE));
	params.farthestDepthInMeters = gTextures[SHADER_TYPES_UPSCALE_FARTHEST_DEPTH_MIP_TEXTURE].SampleLevel(linearClamp, farthestDepthUv, 0.0).r;
	params.newSample = !params.existingSample || !UpscaleHasHistory();
	float4 masks = gTextures[SHADER_TYPES_UPSCALE_REACTIVE_TEXTURE].SampleLevel(linearClamp, params.renderUvSampler, 0.0);
	params.reactiveMask = saturate(masks[kUpscaleReactive]);
	params.disocclusion = saturate(masks[kUpscaleDisocclusion]);
	params.shadingChange = saturate(masks[kUpscaleShadingChange]);
	params.accumulation = saturate(masks[kUpscaleAccumulation]);
	params.accumulation *= float(round(params.accumulation * 100.0) > 1.0);
	return params;
}

// the history's color where the point was (ReprojectHistoryColor), and its lock
void UpscaleReprojectHistoryColor(UpscaleParams params, inout UpscaleData data)
{
	float4 history = UpscaleSampleLanczos2(
		gTextures[SHADER_TYPES_UPSCALE_COLOR_TEXTURE + (UpscaleParity() ^ 1u)], params.reprojectedUv, UpscaleDisplaySize());
	data.historyColor = UpscaleRgbToYCoCg(history.rgb * UpscaleExposure());
	data.lock = history.w;
}

// the lock (UpdateLockStatus): less as the shading changes, the pixel reacts or is revealed, and by a tenth over the
// jitter's sequence a frame; a new lock adds its strength
void UpscaleUpdateLockStatus(UpscaleParams params, inout UpscaleData data)
{
	data.lock *= float(!params.newSample);
	float lifetimeDecreaseFactor = max(saturate(params.shadingChange), max(params.reactiveMask, params.disocclusion));
	data.lock = max(0.0, data.lock - lifetimeDecreaseFactor * kUpscaleLockMax);
	data.lockContributionThisFrame = saturate(saturate(data.lock - kUpscaleLockThreshold) * (kUpscaleLockMax - kUpscaleLockThreshold));
	float newLockIntensity = gTextures[SHADER_TYPES_UPSCALE_NEW_LOCKS_TEXTURE].Load(int3(params.outputPixel, 0)).r *
							 (1.0 - max(params.shadingChange * 0.0, params.reactiveMask));
	data.lock = max(0.0, min(data.lock + newLockIntensity, kUpscaleLockMax));
	// for the next frame
	float lifetimeDecrease = (0.1 / UpscaleJitterSequenceLength()) * (1.0 - lifetimeDecreaseFactor);
	data.lock = max(0.0, data.lock - lifetimeDecrease);
	// none where the point would leave the view next frame
	float2 estimatedUvNextFrame = params.outputUv - params.motion;
	data.lock *= float(UpscaleUvInside(estimatedUvNextFrame, params.view));
}

// how much the history counts against this frame's samples (ComputeBaseAccumulationWeight): the pixel's accumulation,
// less where it moves
void UpscaleBaseAccumulationWeight(UpscaleParams params, inout UpscaleData data)
{
	float baseAccumulation = params.accumulation;
	baseAccumulation = min(baseAccumulation, lerp(baseAccumulation, 0.15, saturate(max(0.0, params.velocity4K * kUpscaleVelocityFactor) / 0.5)));
	data.historyWeight = baseAccumulation;
}

// the 3x3 of the input around where the output pixel is, by a Lanczos 2 kernel as wide as the render pixels are to the
// output's (ComputeUpsampledColorAndWeight: a slice of it, the jitter's others accumulated over frames), and the box of
// their colors; a pixel that hasn't accumulated takes the box's mean, tonemapped
float UpscaleMaxKernelWeight()
{
	float kernelSizeBias = 1.0 + (1.0 / UpscaleDownscaleFactor() - 1.0).x;
	return min(1.99, kernelSizeBias);
}

void UpscaleUpsampledColorAndWeight(UpscaleParams params, inout UpscaleData data)
{
	int2 renderSize = UpscaleRenderSize();
	float2 dstOutputPosition = float2(params.outputPixel) + 0.5;
	float2 srcOutputPosition = dstOutputPosition * UpscaleDownscaleFactor();
	int2 srcInputPosition = int2(floor(srcOutputPosition));
	float2 srcUnjitteredPosition = float2(srcInputPosition) + 0.5 - UpscaleJitter(params.view);
	float2 baseSampleOffset = srcUnjitteredPosition - srcOutputPosition;
	int2 offsetTopLeft;
	offsetTopLeft.x = srcUnjitteredPosition.x > srcOutputPosition.x ? -2 : -1;
	offsetTopLeft.y = srcUnjitteredPosition.y > srcOutputPosition.y ? -2 : -1;
	// with the rows (or columns) flipped where the 4x4 starts two before, so that the 3x3 is always the first 3 rows and
	// columns of the samples
	bool flipRow = srcUnjitteredPosition.y > srcOutputPosition.y;
	bool flipColumn = srcUnjitteredPosition.x > srcOutputPosition.x;
	float2 fOffsetTopLeft = float2(offsetTopLeft);
	bool initialSample = params.accumulation == 0.0;

	float3 samples[9];
	float exposure = UpscaleExposure();
	int sampleIndex = 0;
	for (int row = 0; row < 3; row++)
	{
		for (int column = 0; column < 3; column++)
		{
			int2 sampleColumnRow = int2(flipColumn ? (3 - column) : column, flipRow ? (3 - row) : row);
			int2 sampleCoordinate = UpscaleClampLoad(srcInputPosition + offsetTopLeft + sampleColumnRow, int2(0), renderSize);
			// LoadPreparedColor, tonemapped for an initial sample
			float3 rgb = max(UpscaleInputColor(sampleCoordinate), 0.0) * exposure;
			samples[sampleIndex++] = UpscaleRgbToYCoCg(initialSample ? UpscaleTonemap(rgb) : rgb);
		}
	}

	// how much of each upsampled color this frame takes
	float kernelBiasMaximum = UpscaleMaxKernelWeight();
	float kernelBiasMinimum = max(1.0, (1.0 + kernelBiasMaximum) * 0.3);
	float kernelBiasWeight = min(1.0 - params.disocclusion * 0.5, min(1.0 - params.shadingChange, saturate(data.historyWeight * 5.0)));
	float kernelBias = lerp(kernelBiasMinimum, kernelBiasMaximum, kernelBiasWeight);
	const float kRectificationCurveBias = -2.3;
	sampleIndex = 0;
	for (int row = 0; row < 3; row++)
	{
		for (int column = 0; column < 3; column++)
		{
			int2 sampleColumnRow = int2(flipColumn ? (3 - column) : column, flipRow ? (3 - row) : row);
			float2 offset = fOffsetTopLeft + float2(sampleColumnRow);
			float2 srcSampleOffset = baseSampleOffset + offset;
			int2 srcSamplePosition = srcInputPosition + offsetTopLeft + sampleColumnRow;
			float onScreenFactor = float(UpscaleOnScreen(srcSamplePosition, renderSize));
			if (!initialSample)
			{
				float2 biasedOffset = srcSampleOffset * kernelBias;
				float sampleWeight = onScreenFactor * UpscaleLanczos2ApproxSq(dot(biasedOffset, biasedOffset));
				data.upsampledColor += samples[sampleIndex] * sampleWeight;
				data.upsampledWeight += sampleWeight;
			}
			// the box's
			float boxSampleWeight = exp(kRectificationCurveBias * dot(srcSampleOffset, srcSampleOffset)) * onScreenFactor;
			UpscaleBoxAddSample(row == 0 && column == 0, data.box, samples[sampleIndex], boxSampleWeight);
			sampleIndex++;
		}
	}
	UpscaleBoxComputeVariance(data.box);

	data.upsampledWeight *= float(data.upsampledWeight > kUpscaleEpsilon);
	if (data.upsampledWeight > kUpscaleEpsilon)
	{
		// normalized, and clamped to the box's bounds (deringing)
		data.upsampledColor = data.upsampledColor / data.upsampledWeight;
		data.upsampledWeight *= kUpscaleAverageLanczosWeightPerFrame;
		data.upsampledColor = clamp(data.upsampledColor, data.box.aabbMinimum, data.box.aabbMaximum);
	}
	// the initial sample, by the tonemapped samples' mean
	if (initialSample)
	{
		data.upsampledColor = UpscaleRgbToYCoCg(UpscaleInverseTonemap(UpscaleYCoCgToRgb(data.box.center)));
		data.upsampledWeight = 1.0;
		data.historyWeight = 0.0;
	}
}

// the history clamped onto the ellipsoid of the box's deviations where it is outside it (RectifyHistory): 3 deviations,
// down to 1 where the point moves, is near, hasn't accumulated, reacts or its shading changed, luma counting 1.7 times;
// less where it is locked or its luma flickers
void UpscaleRectifyHistory(UpscaleParams params, inout UpscaleData data)
{
	float velocityFactor = saturate(params.velocity4K / 20.0);
	float distanceFactor = saturate(0.75 - params.farthestDepthInMeters / 20.0);
	float accumulationFactor = 1.0 - params.accumulation;
	float reactiveFactor = sqrt(params.reactiveMask);
	float shadingChangeFactor = params.shadingChange;
	float boxScaleT = max(velocityFactor, max(distanceFactor, max(accumulationFactor, max(reactiveFactor, shadingChangeFactor))));
	float boxScale = lerp(3.0, 1.0, boxScaleT);
	float3 scaledBoxVec = data.box.vec * float3(1.7, 1.0, 1.0) * boxScale;
	float3 clampedScaledBoxVec = max(scaledBoxVec, 1.193e-7);
	float3 transformedHistoryColor = (data.historyColor - data.box.center) / clampedScaledBoxVec;
	if (length(transformedHistoryColor) > 1.0)
	{
		float3 clampedHistoryColor = normalize(transformedHistoryColor);
		float3 finalClampedHistoryColor = clampedHistoryColor * scaledBoxVec + data.box.center;
		// the history kept where it is locked or its luma flickers
		float historyContribution = max(params.lumaInstabilityFactor, data.lockContributionThisFrame) * params.accumulation * (1.0 - params.disocclusion);
		data.historyColor = lerp(finalClampedHistoryColor, data.historyColor, saturate(historyContribution));
	}
}

// this frame's samples blended into the history (Accumulate), tonemapped (HDR_COLOR_INPUT) so that a bright sample
// doesn't flicker
void UpscaleAccumulate(inout UpscaleData data)
{
	// no invalid values where both weights are 0
	data.historyWeight *= float(data.historyWeight > kUpscaleFp16Min);
	data.historyWeight = max(kUpscaleEpsilon, data.historyWeight + data.upsampledWeight);
	float3 upsampled = UpscaleRgbToYCoCg(UpscaleTonemap(UpscaleYCoCgToRgb(data.upsampledColor)));
	float3 history = UpscaleRgbToYCoCg(UpscaleTonemap(UpscaleYCoCgToRgb(data.historyColor)));
	float alpha = saturate(data.upsampledWeight / data.historyWeight);
	data.historyColor = UpscaleInverseTonemap(UpscaleYCoCgToRgb(lerp(history, upsampled, alpha)));
}

// the accumulation (FSR's Accumulate), per output pixel: the history where the point was (Lanczos 2), its lock updated,
// this frame's samples upsampled, the history rectified and this frame blended into it; into the frame's history (rgb
// before exposure, a the lock), which the Sharpen pass maps into the output
[shader("compute")]
[numthreads(16, 16, 1)]
void ComputeUpscaleAccumulate(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	int2 outputPixel = int2(dispatchThreadId.xy);
	if (any(outputPixel >= UpscaleDisplaySize()))
		return;
	UpscaleParams params = UpscaleInitParams(outputPixel);
	UpscaleData data;
	data.upsampledColor = 0.0;
	data.historyColor = 0.0;
	data.historyWeight = 1.0;
	data.upsampledWeight = 0.0;
	data.lock = 0.0;
	data.lockContributionThisFrame = 0.0;

	if (params.existingSample && !params.newSample)
		UpscaleReprojectHistoryColor(params, data);
	UpscaleUpdateLockStatus(params, data);
	UpscaleBaseAccumulationWeight(params, data);
	UpscaleUpsampledColorAndWeight(params, data);
	UpscaleRectifyHistory(params, data);
	UpscaleAccumulate(data);
	data.historyColor = max(data.historyColor / UpscaleExposure(), 0.0);

	gRWTextures[SHADER_TYPES_UPSCALE_COLOR_RW_TEXTURE + UpscaleParity()].Store(uint2(outputPixel), float4(data.historyColor, data.lock));
	// the pixel's new lock taken
	gRWTextures[SHADER_TYPES_UPSCALE_NEW_LOCKS_RW_TEXTURE].Store(uint2(outputPixel), float4(0.0, 0.0, 0.0, 0.0));
}

// the accumulation sharpened into the output (the Sharpen pass): Lottes' robust contrast adaptive sharpening (FSR's
// RCAS), on the tonemapped light: each pixel moved away from its 4 neighbours by the most that takes no channel past 0
// or 1 (of what they span), at most a fifth of its difference from their mean, less by kSharpenStops. the accumulation's
// blend of jittered samples softens what it resolves by about the reconstruction filter's width; this takes that back
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
	Texture2D resolved = gTextures[SHADER_TYPES_UPSCALE_COLOR_TEXTURE + UpscaleParity()];
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
