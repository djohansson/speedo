// constants and view helpers shared by the rest. a part of shaders.slang, which includes it after capi.h and sky.h

static const float kPi = 3.14159265358979;

// the view a pixel of the render target is in, if any, and its index
bool ViewAt(int2 pixel, out ViewData view, out uint index)
{
	for (uint viewIt = 0; viewIt < gPushConstants.viewCount; viewIt++)
	{
		view = gViewData[gPushConstants.frameIndex][viewIt];
		float2 local = (float2(pixel) + 0.5 - view.viewport.xy) / view.viewport.zw;
		if (all(local >= 0.0) && all(local < 1.0))
		{
			index = viewIt;
			return true;
		}
	}
	view = gViewData[gPushConstants.frameIndex][0];
	index = 0;
	return false;
}

bool ViewAt(int2 pixel, out ViewData view)
{
	uint index;
	return ViewAt(pixel, view, index);
}
