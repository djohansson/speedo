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

// the direction of a view's ray through a point of its viewport (normalized device coordinates): from the near plane
// (depth 1, reversed) toward the far (depth 0: at infinity where the view has no far plane), the homogeneous difference of
// the two unprojected, which needs no division by the far's w (0 at infinity) and has the whole frustum as its baseline.
// the near plane and a point twice as far, each divided and then subtracted, gave a direction off by the positions'
// float error over their few centimetres: a thousandth of a radian with the eye 500 m from the origin (or Bistro's
// camera 4000 cm out), stripes across the sky near the horizon from row to row
float3 ViewRay(ViewData view, float2 ndc)
{
	float4 nearPoint = mul(view.inverseViewProjection, float4(ndc, 1.0, 1.0));
	float4 farPoint = mul(view.inverseViewProjection, float4(ndc, 0.0, 1.0));
	return normalize(farPoint.xyz * nearPoint.w - nearPoint.xyz * farPoint.w);
}

// the index of the view a pixel is in, by the views' viewports alone (ViewAt's, without the whole ViewData)
bool ViewIndexAt(int2 pixel, out uint index)
{
	for (uint viewIt = 0; viewIt < gPushConstants.viewCount; viewIt++)
	{
		float4 viewport = gViewData[gPushConstants.frameIndex][viewIt].viewport;
		float2 local = (float2(pixel) + 0.5 - viewport.xy) / viewport.zw;
		if (all(local >= 0.0) && all(local < 1.0))
		{
			index = viewIt;
			return true;
		}
	}
	index = 0;
	return false;
}
