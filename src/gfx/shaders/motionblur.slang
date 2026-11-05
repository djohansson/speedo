// the camera's motion blur (see LensData::motionBlur and the Motion passes in gfx/renderer.cpp): what moves across the
// sensor while the shutter is open is smeared along its way, the shutter's share of a frame's motion (LensData::
// shutterShare: a 1/160 s shutter at 60 frames a second, 37% of the way a point moved since the last frame), the
// points' motion the temporal upscaling's (UpscaleMotion: what moves by the pre-pass's, the rest by the camera's
// reprojection), as McGuire et al. 2012 reconstruct it: each pixel's velocity (ComputeMotionVelocity), each tile's
// longest and the longest among its neighbors (ComputeMotionTiles, ComputeMotionTilesDilate: how far a pixel's blur
// may gather), and the gather along the tile's longest velocity (ComputeMotionBlur: each sample weighed by whether it
// moves far enough to reach the pixel, or the pixel moves far enough to reach it, in front or behind by depth), after
// the depth of field and before the Lens pass, which takes the frame through it. a part of shaders.slang, included after
// dof.slang

static const uint kMotionBlurSampleCount = 12u;

// the frame the blur takes: through the depth of field where it is drawn, else as it is
uint MotionBlurSourceTexture(LensData lens)
{
	return lens.dof != 0u ? SHADER_TYPES_DOF_TEXTURE : LensFrameTexture();
}

// each output pixel's velocity over the shutter, in the output's pixels (the render pixel under it, its motion to the
// last frame in the frame's uv, over the output's size and the shutter's share of the frame's time; at most
// SHADER_TYPES_MOTION_BLUR_MAX), and the depth with it
[shader("compute")]
[numthreads(16, 16, 1)]
void ComputeMotionVelocity(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	RWTexture2D velocity = gRWTextures[SHADER_TYPES_MOTION_VELOCITY_RW_TEXTURE];
	uint2 size;
	velocity.GetDimensions(size.x, size.y);
	uint2 pixel = dispatchThreadId.xy;
	if (any(pixel >= size))
		return;
	LensData lens = gLensData[gPushConstants.frameIndex][0];
	int2 renderSize = UpscaleRenderSize();
	int2 at = clamp(int2((float2(pixel) + 0.5) * float2(renderSize) / float2(size)), int2(0, 0), renderSize - 1);
	float depth = gTextures[SHADER_TYPES_MAIN_DEPTH_TEXTURE].Load(int3(at, 0)).r;
	float2 motion = UpscaleMotion(at, depth) * float2(size) * lens.shutterShare;
	float length2 = dot(motion, motion);
	if (length2 > SHADER_TYPES_MOTION_BLUR_MAX * SHADER_TYPES_MOTION_BLUR_MAX)
		motion *= SHADER_TYPES_MOTION_BLUR_MAX / sqrt(length2);
	velocity[pixel] = float4(motion, depth, 0.0);
}

// each tile's longest velocity (the vector), of every other pixel's
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMotionTiles(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	RWTexture2D tiles = gRWTextures[SHADER_TYPES_MOTION_TILES_RW_TEXTURE];
	uint2 tileCount;
	tiles.GetDimensions(tileCount.x, tileCount.y);
	uint2 tile = dispatchThreadId.xy;
	if (any(tile >= tileCount))
		return;
	Texture2D velocity = gTextures[SHADER_TYPES_MOTION_VELOCITY_TEXTURE];
	uint2 size;
	velocity.GetDimensions(size.x, size.y);
	float2 longest = 0.0;
	float longestLength2 = 0.0;
	for (uint y = 0; y < SHADER_TYPES_MOTION_BLUR_TILE_SIZE; y += 2u)
		for (uint x = 0; x < SHADER_TYPES_MOTION_BLUR_TILE_SIZE; x += 2u)
		{
			uint2 pixel = tile * SHADER_TYPES_MOTION_BLUR_TILE_SIZE + uint2(x, y);
			if (any(pixel >= size))
				continue;
			float2 there = velocity.Load(int3(pixel, 0)).xy;
			float length2 = dot(there, there);
			if (length2 > longestLength2)
			{
				longest = there;
				longestLength2 = length2;
			}
		}
	tiles[tile] = float4(longest, 0.0, 0.0);
}

// each tile's longest among its neighbors within the longest's reach (4 tiles each way at most)
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMotionTilesDilate(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	RWTexture2D dilated = gRWTextures[SHADER_TYPES_MOTION_TILES_DILATED_RW_TEXTURE];
	uint2 tileCount;
	dilated.GetDimensions(tileCount.x, tileCount.y);
	uint2 tile = dispatchThreadId.xy;
	if (any(tile >= tileCount))
		return;
	Texture2D tiles = gTextures[SHADER_TYPES_MOTION_TILES_TEXTURE];
	float2 longest = 0.0;
	float longestLength2 = 0.0;
	for (int y = -4; y <= 4; y++)
		for (int x = -4; x <= 4; x++)
		{
			int2 at = clamp(int2(tile) + int2(x, y), int2(0, 0), int2(tileCount) - 1);
			float2 there = tiles.Load(int3(at, 0)).xy;
			float length2 = dot(there, there);
			// only as far as that tile's blur reaches
			float reach = sqrt(length2) / float(SHADER_TYPES_MOTION_BLUR_TILE_SIZE) + 1.0;
			if (length2 > longestLength2 && float(max(abs(x), abs(y))) <= reach)
			{
				longest = there;
				longestLength2 = length2;
			}
		}
	dilated[tile] = float4(longest, 0.0, 0.0);
}

// McGuire's weights: a sample moving (or the pixel moving) far enough to cover the distance between them, soft over a
// pixel; and whether one is in front of the other, soft over a share of their distances
float MotionCone(float distance, float speed)
{
	return saturate(1.0 - distance / max(speed, 1e-3));
}

float MotionCylinder(float distance, float speed)
{
	return 1.0 - smoothstep(0.95 * speed, 1.05 * speed + 1.0, distance);
}

float MotionInFront(float depthA, float depthB)
{
	// the depths reversed: the greater nearer; soft over a hundredth
	return saturate(1.0 - (depthB - depthA) / max(0.01 * max(depthA, depthB), 1e-6));
}

// the gather: along the tile's longest velocity through the pixel, kMotionBlurSampleCount samples jittered per pixel,
// each weighed as above; the pixel itself by its own speed's cone over a pixel
[shader("compute")]
[numthreads(16, 16, 1)]
void ComputeMotionBlur(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	RWTexture2D output = gRWTextures[SHADER_TYPES_MOTION_BLUR_RW_TEXTURE];
	uint2 size;
	output.GetDimensions(size.x, size.y);
	uint2 pixel = dispatchThreadId.xy;
	if (any(pixel >= size))
		return;
	LensData lens = gLensData[gPushConstants.frameIndex][0];
	Texture2D source = gTextures[MotionBlurSourceTexture(lens)];
	Texture2D velocity = gTextures[SHADER_TYPES_MOTION_VELOCITY_TEXTURE];
	Texture2D tiles = gTextures[SHADER_TYPES_MOTION_TILES_DILATED_TEXTURE];
	float4 center = source.Load(int3(pixel, 0));
	float2 longest = tiles.Load(int3(pixel / SHADER_TYPES_MOTION_BLUR_TILE_SIZE, 0)).xy;
	float longestSpeed = length(longest);
	if (longestSpeed < 0.5)
	{
		output[pixel] = center;
		return;
	}
	float3 centerVelocity = velocity.Load(int3(pixel, 0)).xyz;
	float centerSpeed = length(centerVelocity.xy);
	float centerDepth = centerVelocity.z;
	float2 direction = longest / longestSpeed;
	float jitter = DofNoise(float2(pixel) + float(lens.grainFrame % 8u)) - 0.5;
	float3 sum = center.rgb;
	float weight = 1.0;
	for (uint sampleIt = 0; sampleIt < kMotionBlurSampleCount; sampleIt++)
	{
		float t = lerp(-1.0, 1.0, (float(sampleIt) + 0.5 + jitter) / float(kMotionBlurSampleCount));
		float distance = abs(t) * longestSpeed;
		if (distance < 0.5)
			continue;
		int2 at = clamp(int2(floor(float2(pixel) + 0.5 + direction * t * longestSpeed)), int2(0, 0), int2(size) - 1);
		float3 sampleVelocity = velocity.Load(int3(at, 0)).xyz;
		float sampleSpeed = length(sampleVelocity.xy);
		float front = MotionInFront(centerDepth, sampleVelocity.z);
		float behind = MotionInFront(sampleVelocity.z, centerDepth);
		float sampleWeight = behind * MotionCone(distance, sampleSpeed) + front * MotionCone(distance, centerSpeed) +
							 2.0 * MotionCylinder(distance, sampleSpeed) * MotionCylinder(distance, centerSpeed);
		sum += source.Load(int3(at, 0)).rgb * sampleWeight;
		weight += sampleWeight;
	}
	output[pixel] = float4(sum / weight, center.a);
}
