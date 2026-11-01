// texture views (texcoord set, transform, sampler), tangent frames and normal maps. a part of shaders.slang, which includes it after capi.h and sky.h

// the screen space derivatives of the surface's position and texcoords where there are no implicit ones (the visibility
// buffer's shading, in compute: analytic, from the pixel's triangle, see ComputeMainVisibility), which the texture
// lookups and the tangent frames take there (see SampleView, CotangentFrame)
static float3 gPositionDx;
static float3 gPositionDy;
static float4 gTexCoordDx;
static float4 gTexCoordDy;

// a view's texcoords' derivative from its set's (see ViewTexCoord: its transform's linear part)
float2 ViewTexCoordDerivative(TextureView view, float4 texCoord01Derivative)
{
	float2 d = view.texCoordSet == 0u ? texCoord01Derivative.xy : texCoord01Derivative.zw;
	return float2(dot(view.uTransform.xy, d), dot(view.vTransform.xy, d));
}

// a surface's screen space derivatives at a pixel along x and y: its position's and a view's texcoords' (the fragment
// stage's implicit ones, else gPositionDx's)
struct SurfaceDerivatives
{
	float3 dp1;
	float3 dp2;
	float2 duv1;
	float2 duv2;
};

SurfaceDerivatives SurfaceDerivativesAt(float3 position, TextureView view, float4 texCoord01)
{
	SurfaceDerivatives result;
	__stage_switch
	{
	case fragment:
		{
			float2 uv = ViewTexCoord(view, texCoord01);
			result.dp1 = ddx(position);
			result.dp2 = ddy(position);
			result.duv1 = ddx(uv);
			result.duv2 = ddy(uv);
			return result;
		}
	case compute:
		{
			result.dp1 = gPositionDx;
			result.dp2 = gPositionDy;
			result.duv1 = ViewTexCoordDerivative(view, gTexCoordDx);
			result.duv2 = ViewTexCoordDerivative(view, gTexCoordDy);
			return result;
		}
	}
	// (no other stage shades a surface)
	return result;
}

// the tangent frame at a pixel, from the screen space derivatives of its position and a view's texcoords (Christian
// Schüler, "Normal Mapping Without Precomputed Tangents"), so that meshes need no tangents. rows: along +u, along +v,
// normal.
float3x3 CotangentFrame(float3 normal, float3 position, TextureView view, float4 texCoord01)
{
	SurfaceDerivatives d = SurfaceDerivativesAt(position, view, texCoord01);
	float3 dp1 = d.dp1;
	float3 dp2 = d.dp2;
	float2 duv1 = d.duv1;
	float2 duv2 = d.duv2;

	float3 dp2perp = cross(dp2, normal);
	float3 dp1perp = cross(normal, dp1);
	float3 tangent = dp2perp * duv1.x + dp1perp * duv2.x;
	float3 bitangent = dp2perp * duv1.y + dp1perp * duv2.y;

	// the solution is scaled by dot(cross(dp1, dp2), normal), whose sign depends on which way the screen's y axis points
	// (it is negative for front faces here, where y points down), so the frame needs that sign to point along +u and +v
	float handedness = dot(cross(dp1, dp2), normal) < 0.0 ? -1.0 : 1.0;
	float invmax = handedness * rsqrt(max(max(dot(tangent, tangent), dot(bitangent, bitangent)), 1e-20));
	return float3x3(tangent * invmax, bitangent * invmax, normal);
}

// the tangent frame at a pixel from the vertex tangent (see VertexP3fN3fTa4fT014fC4f::tangent), already made
// perpendicular to the normal, turned to follow the view's transform of the texcoords: with uv' = A * uv + c, +u' and
// +v' point along the columns of A^-1 applied to (+u, +v). rows as CotangentFrame's.
float3x3 TangentFrame(float3 normal, float3 alongU, float handedness, TextureView view)
{
	float3 alongV = -cross(normal, alongU) * sign(handedness);

	float det = view.uTransform.x * view.vTransform.y - view.uTransform.y * view.vTransform.x;
	if (det != 0.0)
	{
		float s = sign(det);
		float3 u = normalize((alongU * view.vTransform.y - alongV * view.vTransform.x) * s);
		float3 v = normalize((alongV * view.uTransform.x - alongU * view.uTransform.y) * s);
		alongU = u;
		alongV = v;
	}
	return float3x3(alongU, alongV, normal);
}

// a view's texcoords: its set, transformed (see TextureView)
float2 ViewTexCoord(TextureView view, float4 texCoord01)
{
	float3 uv1 = float3(view.texCoordSet == 0u ? texCoord01.xy : texCoord01.zw, 1.0);
	return float2(dot(view.uTransform.xyz, uv1), dot(view.vTransform.xyz, uv1));
}

// a view's texture at its texcoords (ViewTexCoord's: the derivatives where there are no implicit ones are taken through
// the view's transform, see gTexCoordDx), its mips chosen for the output's size (ViewData::textureLodBias)
float4 SampleView(TextureView view, float2 uv)
{
	float bias = gViewData[gPushConstants.frameIndex][gShadingViewId].textureLodBias;
	__stage_switch
	{
	case fragment:
		return gTextures[view.textureId].SampleBias(gSamplers[view.samplerId], uv, bias);
	case compute:
		{
			// a bias of b is the derivatives 2^b times as long
			float scale = exp2(bias);
			return gTextures[view.textureId].SampleGrad(
				gSamplers[view.samplerId], uv, ViewTexCoordDerivative(view, gTexCoordDx) * scale, ViewTexCoordDerivative(view, gTexCoordDy) * scale);
		}
	}
	return 0.0;
}

float4 SampleView(uint viewId, float4 texCoord01)
{
	TextureView view = gTextureViews[viewId];
	return SampleView(view, ViewTexCoord(view, texCoord01));
}

// the tangent frame (rows: along +u, along +v as sampled, normal) of a texture view's texcoords at a pixel: the vertex
// tangents' where they are valid, else one from screen space derivatives (see NormalFromMap)
float3x3 SurfaceFrame(float3 normal, float4 tangent, float3 position, float4 texCoord01, TextureView view, float faceSign)
{
	float3 alongU = tangent.xyz - normal * dot(normal, tangent.xyz);
	if (abs(tangent.w) > 0.5 && dot(alongU, alongU) > 1e-4 * dot(tangent.xyz, tangent.xyz))
	{
		float3x3 frame = TangentFrame(normal, normalize(alongU), tangent.w, view);
		return float3x3(frame[0], frame[1], faceSign * normal);
	}
	return CotangentFrame(faceSign * normal, position, view, texCoord01);
}

// a normal map's normal (x and y in rg, see MaterialData::normalView), by a scale of x and y, with normal the (unit)
// vertex normal: in the vertex tangents' frame where they are valid (interpolated w stays away from 0, and they aren't
// nearly parallel to the normal, which some files have), else a frame from screen space derivatives. the frame follows
// the map's own (transformed) texcoords. faceSign reverses it for back faces.
float3 NormalFromMap(uint viewId, float scale, float3 normal, float4 tangent, float3 position, float4 texCoord01, float faceSign)
{
	TextureView normalView = gTextureViews[viewId];
	float2 uv = ViewTexCoord(normalView, texCoord01);
	float2 xy = SampleView(normalView, uv).rg * 2.0 - 1.0;
	if ((normalView.flags & TEXTURE_VIEW_FLAG_NORMAL_Y_UP) != 0u)
		xy.y = -xy.y;
	// z is reconstructed from the stored (unit length) normal, then x and y are scaled, as gltf defines
	// normalTexture.scale: normalize(normal * (scale, scale, 1))
	float3 tangentSpaceNormal = float3(xy * scale, sqrt(saturate(1.0 - dot(xy, xy))));
	float3 alongU = tangent.xyz - normal * dot(normal, tangent.xyz);
	if (abs(tangent.w) > 0.5 && dot(alongU, alongU) > 1e-4 * dot(tangent.xyz, tangent.xyz))
		return faceSign * normalize(mul(tangentSpaceNormal, TangentFrame(normal, normalize(alongU), tangent.w, normalView)));
	return normalize(mul(tangentSpaceNormal, CotangentFrame(faceSign * normal, position, normalView, texCoord01)));
}
