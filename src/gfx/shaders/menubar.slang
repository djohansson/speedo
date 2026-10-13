// the menu bar's frosted backdrop, as macOS's: the output under the ui's menu bar (ComputeMain wrote it, linear, to
// SHADER_TYPES_MENU_BAR_SOURCE_TEXTURE) blurred by a gaussian and a little more saturated, then written back over the
// output, before the ui draws its translucent bar over it. a part of shaders.slang, which includes it after capi.h and
// sky.h

static const int kMenuBarBlurRadius = int(SHADER_TYPES_MENU_BAR_BLUR_RADIUS);

// a gaussian's weight at offset pixels, whose radius is about 2.5 standard deviations
float MenuBarBlurWeight(int offset)
{
	float sigma = float(kMenuBarBlurRadius) / 2.5;
	return exp(-0.5 * float(offset * offset) / (sigma * sigma));
}

// the Menu Bar Blur X pass: the source's rows (under the bar, and the blur's reach below it) blurred along x
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMenuBarBlurX(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	Texture2D source = gTextures[SHADER_TYPES_MENU_BAR_SOURCE_TEXTURE];
	uint2 size;
	source.GetDimensions(size.x, size.y);
	uint2 xy = dispatchThreadId.xy;
	if (xy.x >= size.x || xy.y >= min(gPushConstants.menuBarHeight + SHADER_TYPES_MENU_BAR_BLUR_RADIUS, size.y))
		return;

	float3 sum = 0.0;
	float weights = 0.0;
	for (int offset = -kMenuBarBlurRadius; offset <= kMenuBarBlurRadius; offset++)
	{
		int x = clamp(int(xy.x) + offset, 0, int(size.x) - 1);
		float weight = MenuBarBlurWeight(offset);
		sum += source.Load(int3(x, xy.y, 0)).rgb * weight;
		weights += weight;
	}
	gRWTextures[SHADER_TYPES_MENU_BAR_ROWS_RW_TEXTURE].Store(xy, float4(sum / weights, 1.0));
}

// the Menu Bar Blur Y pass: those rows blurred along y (from the rows below the bar too), over the output under it
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMenuBarBlurY(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	Texture2D rows = gTextures[SHADER_TYPES_MENU_BAR_ROWS_TEXTURE];
	RWTexture2D output = gRWTextures[gPushConstants.frameIndex];
	uint2 size;
	output.GetDimensions(size.x, size.y);
	uint2 xy = dispatchThreadId.xy;
	if (xy.x >= size.x || xy.y >= min(gPushConstants.menuBarHeight, size.y))
		return;

	int lastRow = int(min(gPushConstants.menuBarHeight + SHADER_TYPES_MENU_BAR_BLUR_RADIUS, size.y)) - 1;
	float3 sum = 0.0;
	float weights = 0.0;
	for (int offset = -kMenuBarBlurRadius; offset <= kMenuBarBlurRadius; offset++)
	{
		int y = clamp(int(xy.y) + offset, 0, lastRow);
		float weight = MenuBarBlurWeight(offset);
		sum += rows.Load(int3(xy.x, y, 0)).rgb * weight;
		weights += weight;
	}
	float3 color = sum / weights;
	// macOS's vibrancy: the colors behind come through a little stronger
	float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
	color = saturate(lerp(float3(luminance), color, 1.3));
	output.Store(xy, float4(ApplySRGBCurve(color), 1.0));
}
