// constants and view helpers shared by the rest. a part of shaders.slang, which includes it after capi.h and sky.h

static const float kPi = 3.14159265358979;

// the view a pixel of the render target is in, if any
bool ViewAt(int2 pixel, out ViewData view)
{
	for (uint viewIt = 0; viewIt < gPushConstants.viewCount; viewIt++)
	{
		view = gViewData[gPushConstants.frameIndex][viewIt];
		float2 local = (float2(pixel) + 0.5 - view.viewport.xy) / view.viewport.zw;
		if (all(local >= 0.0) && all(local < 1.0))
			return true;
	}
	view = gViewData[gPushConstants.frameIndex][0];
	return false;
}
