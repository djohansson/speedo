module speedo;

__include types;

#include "capi.h"

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_GLOBAL)]]
[[vk::push_constant]]
ConstantBuffer<PushConstants> gPushConstants;

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS)]]
StructuredBuffer<VertexP3fN3fTa4fT014fC4f> gVertexBuffer;

[[vk::binding(1, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS)]]
StructuredBuffer<SkinVertex> gSkinVertices;

[[vk::binding(2, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS)]]
StructuredBuffer<MorphDelta> gMorphDeltas;

// order independent transparency (see OitNode)
[[vk::binding(3, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS)]]
RWStructuredBuffer<uint> gOitHeads;
[[vk::binding(4, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS)]]
RWStructuredBuffer<OitNode> gOitNodes;
[[vk::binding(5, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS)]]
RWStructuredBuffer<uint> gOitCounter;

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES)]]
Texture2D gTextures[SHADER_TYPES_GLOBAL_TEXTURE_COUNT];

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES)]]
RWTexture2D gRWTextures[SHADER_TYPES_GLOBAL_RW_TEXTURE_COUNT];

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS)]]
SamplerState gSamplers[SHADER_TYPES_GLOBAL_SAMPLER_COUNT];

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_VIEW)]]
StructuredBuffer<ViewData> gViewData[SHADER_TYPES_FRAME_COUNT];

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_MATERIAL)]]
StructuredBuffer<MaterialData> gMaterialData;

[[vk::binding(1, DESCRIPTOR_SET_CATEGORY_MATERIAL)]]
StructuredBuffer<TextureView> gTextureViews;

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<ModelInstance> gModelInstances[SHADER_TYPES_FRAME_COUNT];

[[vk::binding(1, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<LightData> gLights;

[[vk::binding(2, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<float4x4> gJointMatrices[SHADER_TYPES_FRAME_COUNT];

[[vk::binding(3, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<float> gMorphWeights[SHADER_TYPES_FRAME_COUNT];

[[vk::binding(4, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<EnvironmentData> gEnvironment;

// a vertex moved by its submesh's morph targets: each target's deltas (see MorphDelta) times the target's weight
void Morph(uint vertexId, uint frameIndex, inout float3 position, inout float3 normal, inout float3 tangent)
{
	uint count = gPushConstants.morphTargetCount;
	uint row = gPushConstants.morphDeltaBase + (vertexId - gPushConstants.morphFirstVertex) * count;
	for (uint t = 0; t < count; t++)
	{
		float weight = gMorphWeights[frameIndex][gPushConstants.morphWeightBase + t];
		if (weight == 0.0)
			continue;
		MorphDelta delta = gMorphDeltas[row + t];
		position += weight * delta.position.xyz;
		normal += weight * delta.normal.xyz;
		tangent += weight * delta.tangent.xyz;
	}
}

// a skinned vertex's matrix: its joints' matrices by its weights (see SkinVertex), from mesh to world space
float4x4 SkinMatrix(uint vertexId, uint frameIndex, uint jointBase)
{
	SkinVertex skin = gSkinVertices[vertexId];
	float4x4 result = 0;
	for (uint i = 0; i < 4; i++)
	{
		uint shift = (i % 2) * 16;
		uint joint = (skin.joints[i / 2] >> shift) & 0xffffu;
		float weight = float((skin.weights[i / 2] >> shift) & 0xffffu) / 65535.0;
		if (weight > 0.0)
			result += weight * gJointMatrices[frameIndex][jointBase + joint];
	}
	return result;
}

struct InputStream : IInputStream
{
	uint vertexId : SV_VertexID;
	uint instanceId : SV_InstanceID;
};

struct VertexZPrepassOutput : IInterpolants
{
	float4 hPos : SV_Position;
};

struct VertexOutput : IInterpolants
{
	VertexP3fN3fTa4fT014fC4f vertex;
	float4 hPos : SV_Position;
};

// VertexOutput, and what only the vertex stage writes (a system value the fragment stage can't take as input)
struct VertexMainOutput
{
	VertexOutput varyings;
	// points are a pixel (vulkan needs the size written for point lists)
	float pointSize : SV_PointSize;
};

// the main pass's color attachment, which opaque draws write (blended ones go in the per pixel lists instead, see
// FragmentTransparent)
struct FragmentOutput
{
	float4 color : SV_Target;
};

// non-negative colors as R11G11B10 floats (half floats without their sign and lowest mantissa bits)
uint PackR11G11B10(float3 color)
{
	uint3 bits = f32tof16(clamp(color, 0.0, 65000.0));
	return ((bits.r >> 4) & 0x7ffu) | (((bits.g >> 4) & 0x7ffu) << 11) | (((bits.b >> 5) & 0x3ffu) << 22);
}

float3 UnpackR11G11B10(uint packed)
{
	return f16tof32(uint3((packed & 0x7ffu) << 4, ((packed >> 11) & 0x7ffu) << 4, ((packed >> 22) & 0x3ffu) << 5));
}

float3 SignedOctEncode(float3 n)
{
	float3 OutN;

	n /= (abs(n.x) + abs(n.y) + abs(n.z));

	OutN.y = n.y * 0.5 + 0.5;
	OutN.x = n.x * 0.5 + OutN.y;
	OutN.y = n.x * -0.5 + OutN.y;
	OutN.z = saturate(n.z*FLT_MAX);

	return OutN;
}

float3 SignedOctDecode(float3 n)
{
	float3 OutN;

	OutN.x = (n.x - n.y);
	OutN.y = (n.x + n.y) - 1.0;
	OutN.z = n.z * 2.0 - 1.0;
	OutN.z = OutN.z * (1.0 - abs(OutN.x) - abs(OutN.y));
 
	return normalize(OutN);
}

[shader("vertex")]
VertexZPrepassOutput VertexZPrepass(InputStream input)
{
	VertexZPrepassOutput output;

	uint frameIndex = gPushConstants.frameIndex;
	uint viewAndMaterialId = gPushConstants.viewAndMaterialId;
	uint modelInstanceId = gPushConstants.modelInstanceId;
	
	uint viewId = viewAndMaterialId >> SHADER_TYPES_MATERIAL_INDEX_BITS;

	ViewData view = gViewData[frameIndex][viewId];

	VertexP3fN3fTa4fT014fC4f v = gVertexBuffer[input.vertexId];
	ModelInstance i = gModelInstances[frameIndex][modelInstanceId + input.instanceId];

	float3 position = v.position;
	if (gPushConstants.morphTargetCount > 0)
	{
		float3 normal = v.normal;
		float3 tangent = v.tangent.xyz;
		Morph(input.vertexId, frameIndex, position, normal, tangent);
	}
	if (gPushConstants.jointBase != SHADER_TYPES_NOT_SKINNED)
		position = mul(SkinMatrix(input.vertexId, frameIndex, gPushConstants.jointBase), float4(position, 1.0)).xyz;
	float3 worldPosition = mul(i.modelTransform, float4(position, 1.0)).xyz;
	
	output.hPos = mul(view.viewProjection, float4(worldPosition, 1.0));
	
	return output;
}

[shader("vertex")]
VertexMainOutput VertexMain(InputStream input)
{
	VertexOutput output;

	uint frameIndex = gPushConstants.frameIndex;
	uint viewAndMaterialId = gPushConstants.viewAndMaterialId;
	uint modelInstanceId = gPushConstants.modelInstanceId;

	uint viewId = viewAndMaterialId >> SHADER_TYPES_MATERIAL_INDEX_BITS;

	ViewData view = gViewData[frameIndex][viewId];

	VertexP3fN3fTa4fT014fC4f v = gVertexBuffer[input.vertexId];
	ModelInstance i = gModelInstances[frameIndex][modelInstanceId + input.instanceId];

	// morph targets move the vertices first (in the space they are stored in), then skinned vertices move with their joints (in world space), before the instance's transform (the identity for them).
	// the normals and tangents go by the same matrix, which is exact for joints without non-uniform scale.
	float3 position = v.position;
	float3 normal = v.normal;
	float3 tangent = v.tangent.xyz;
	if (gPushConstants.morphTargetCount > 0)
		Morph(input.vertexId, frameIndex, position, normal, tangent);
	if (gPushConstants.jointBase != SHADER_TYPES_NOT_SKINNED)
	{
		float4x4 skin = SkinMatrix(input.vertexId, frameIndex, gPushConstants.jointBase);
		position = mul(skin, float4(position, 1.0)).xyz;
		normal = mul(skin, float4(normal, 0.0)).xyz;
		tangent = mul(skin, float4(tangent, 0.0)).xyz;
	}

	float3 worldPosition = mul(i.modelTransform, float4(position, 1.0)).xyz;
	float3 worldNormal = mul(i.inverseTransposeModelTransform, float4(normal, 0.0)).xyz;

	output.hPos = mul(view.viewProjection, float4(worldPosition, 1.0));
	output.vertex.position = worldPosition;
	output.vertex.normal = worldNormal;
	// a mirroring transform flips the handedness of the tangent frame
	float3x3 linear = float3x3(i.modelTransform[0].xyz, i.modelTransform[1].xyz, i.modelTransform[2].xyz);
	output.vertex.tangent = float4(mul(linear, tangent), determinant(linear) < 0.0 ? -v.tangent.w : v.tangent.w);
	output.vertex.texCoord01 = v.texCoord01;
	output.vertex.color = v.color;

	return { output, 1.0 };
}

// the tangent frame at a pixel, from the screen space derivatives of its position and texcoords (Christian Schüler,
// "Normal Mapping Without Precomputed Tangents"), so that meshes need no tangents. rows: along +u, along +v, normal.
float3x3 CotangentFrame(float3 normal, float3 position, float2 uv)
{
	float3 dp1 = ddx(position);
	float3 dp2 = ddy(position);
	float2 duv1 = ddx(uv);
	float2 duv2 = ddy(uv);

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

float4 SampleView(TextureView view, float2 uv)
{
	return gTextures[view.textureId].Sample(gSamplers[view.samplerId], uv);
}

float4 SampleView(uint viewId, float4 texCoord01)
{
	TextureView view = gTextureViews[viewId];
	return SampleView(view, ViewTexCoord(view, texCoord01));
}

static const float kPi = 3.14159265358979;

// the environment (see EnvironmentData): a world direction in the panorama's space, turned back by its rotation
float3 EnvironmentDirection(float3 d)
{
	float angle = -gPushConstants.environmentRotation;
	float c = cos(angle);
	float s = sin(angle);
	return float3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
}

// where a direction (in the panorama's space) is in the panorama (as gfx::environment::Panorama)
float2 EnvironmentCoordinates(float3 d)
{
	return float2(0.5 + atan2(d.x, -d.z) / (2.0 * kPi), acos(clamp(d.y, -1.0, 1.0)) / kPi);
}

// the radiance a white Lambertian surface of normal n reflects (as gfx::environment::EvaluateIrradiance), scaled by the
// intensity
float3 EnvironmentIrradiance(EnvironmentData environment, float3 n)
{
	n = EnvironmentDirection(n);
	float basis[9] = {
		0.282095,
		0.488603 * n.y,
		0.488603 * n.z,
		0.488603 * n.x,
		1.092548 * n.x * n.y,
		1.092548 * n.y * n.z,
		0.315392 * (3.0 * n.z * n.z - 1.0),
		1.092548 * n.x * n.z,
		0.546274 * (n.x * n.x - n.y * n.y),
	};
	float3 result = 0.0;
	for (uint i = 0; i < 9; i++)
		result += environment.irradiance[i].rgb * basis[i];
	return max(result, 0.0) * gPushConstants.environmentIntensity;
}

// the radiance reflected along r, prefiltered for a roughness (the split sum's first term), scaled by the intensity.
// roughness 0 is the panorama itself, as the backdrop shows it
float3 EnvironmentRadiance(EnvironmentData environment, float3 r, float roughness)
{
	float lod = roughness * (environment.levelCount - 1.0);
	float2 uv = EnvironmentCoordinates(EnvironmentDirection(r));
	return gTextures[environment.textureId].SampleLevel(gSamplers[environment.samplerId], uv, lod).rgb * gPushConstants.environmentIntensity;
}

// the glTF metallic-roughness brdf (the spec's appendix B): GGX distribution, height correlated Smith visibility, and
// Schlick's Fresnel, mixing a Lambertian diffuse and the specular by the Fresnel term
float3 FresnelSchlick(float3 f0, float f90, float cosine)
{
	return f0 + (f90 - f0) * pow(saturate(1.0 - cosine), 5.0);
}

float DistributionGgx(float nDotH, float alpha)
{
	float alpha2 = alpha * alpha;
	float d = nDotH * nDotH * (alpha2 - 1.0) + 1.0;
	return alpha2 / (kPi * d * d);
}

float VisibilitySmithGgxCorrelated(float nDotL, float nDotV, float alpha)
{
	float alpha2 = alpha * alpha;
	float ggxV = nDotL * sqrt(nDotV * nDotV * (1.0 - alpha2) + alpha2);
	float ggxL = nDotV * sqrt(nDotL * nDotL * (1.0 - alpha2) + alpha2);
	float sum = ggxV + ggxL;
	return sum > 0.0 ? 0.5 / sum : 0.0;
}

// the specular reflectance of a uniform environment, integrated over the hemisphere: Karis' analytic fit to the
// split sum's lookup table ("Physically Based Shading on Mobile")
float3 EnvironmentBrdfApprox(float3 f0, float f90, float roughness, float nDotV)
{
	const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
	const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
	float4 r = roughness * c0 + c1;
	float a004 = min(r.x * r.x, exp2(-9.28 * nDotV)) * r.x + r.y;
	float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
	return f0 * ab.x + f90 * ab.y;
}

// KHR_materials_sheen's lobe: the Charlie distribution and Estevez and Kulla's visibility (as the Khronos sample viewer
// has them: interpolated by alpha, the visibility at most 1), of a roughness at least 0.07
float SheenAlpha(float roughness)
{
	float clamped = max(roughness, 0.07);
	return clamped * clamped;
}

float DistributionCharlie(float nDotH, float roughness)
{
	float alpha = SheenAlpha(roughness);
	float sin2 = max(1.0 - nDotH * nDotH, 0.0);
	return (2.0 + 1.0 / alpha) * pow(sin2, 0.5 / alpha) / (2.0 * kPi);
}

float SheenLambda(float cosine, float alpha)
{
	float t = (1.0 - alpha) * (1.0 - alpha);
	float a = lerp(25.3245, 21.5473, t);
	float b = lerp(3.32435, 3.82987, t);
	float c = lerp(0.16801, 0.19823, t);
	float d = lerp(-1.27393, -1.97760, t);
	float e = lerp(-4.85967, -4.32054, t);
	float x = cosine < 0.5 ? cosine : 0.5;
	float l = a / (1.0 + b * pow(x, c)) + d * x + e;
	if (cosine < 0.5)
		return exp(l);
	float x1 = 1.0 - cosine;
	return exp(2.0 * l - (a / (1.0 + b * pow(x1, c)) + d * x1 + e));
}

float VisibilitySheen(float nDotL, float nDotV, float roughness)
{
	float alpha = SheenAlpha(roughness);
	return saturate(1.0 / ((1.0 + SheenLambda(nDotV, alpha) + SheenLambda(nDotL, alpha)) * 4.0 * nDotV * nDotL));
}

// the sheen lobe's directional albedo (white): integrated numerically for cos 0 to 1 (columns) and roughness 0 to 1
// (rows), 12 each, at most 1 (grazing views of the smoothest sheens exceed it). what the layers below lose to it
static const float kSheenAlbedo[144] = {
	1.0000, 0.6230, 0.1468, 0.0241, 0.0024, 0.0001, 0.0000, 0.0000, 0.0000, 0.0000, 0.0000, 0.0000,
	1.0000, 0.8200, 0.2877, 0.0804, 0.0165, 0.0024, 0.0002, 0.0000, 0.0000, 0.0000, 0.0000, 0.0000,
	1.0000, 1.0000, 0.6423, 0.3549, 0.1788, 0.0833, 0.0355, 0.0135, 0.0042, 0.0010, 0.0001, 0.0000,
	1.0000, 0.9785, 0.6775, 0.4508, 0.2882, 0.1790, 0.1070, 0.0607, 0.0317, 0.0144, 0.0049, 0.0007,
	0.9801, 0.8316, 0.6178, 0.4494, 0.3218, 0.2274, 0.1573, 0.1061, 0.0682, 0.0404, 0.0205, 0.0070,
	0.7804, 0.6928, 0.5361, 0.4115, 0.3153, 0.2401, 0.1804, 0.1339, 0.0966, 0.0661, 0.0412, 0.0211,
	0.6296, 0.5733, 0.4550, 0.3625, 0.2906, 0.2324, 0.1844, 0.1456, 0.1131, 0.0850, 0.0601, 0.0381,
	0.5157, 0.4748, 0.3833, 0.3140, 0.2598, 0.2150, 0.1772, 0.1463, 0.1199, 0.0961, 0.0741, 0.0534,
	0.4310, 0.3971, 0.3244, 0.2714, 0.2298, 0.1949, 0.1652, 0.1410, 0.1202, 0.1012, 0.0831, 0.0652,
	0.3704, 0.3396, 0.2801, 0.2379, 0.2049, 0.1771, 0.1532, 0.1341, 0.1178, 0.1030, 0.0886, 0.0741,
	0.3306, 0.3019, 0.2510, 0.2156, 0.1880, 0.1648, 0.1448, 0.1291, 0.1161, 0.1044, 0.0931, 0.0817,
	0.3104, 0.2841, 0.2381, 0.2064, 0.1817, 0.1609, 0.1431, 0.1294, 0.1183, 0.1086, 0.0994, 0.0902,
};

float SheenAlbedo(float cosine, float roughness)
{
	float2 at = saturate(float2(cosine, roughness)) * 11.0;
	uint2 low = min(uint2(at), uint2(10, 10));
	float2 t = at - float2(low);
	float a = lerp(kSheenAlbedo[low.y * 12 + low.x], kSheenAlbedo[low.y * 12 + low.x + 1], t.x);
	float b = lerp(kSheenAlbedo[(low.y + 1) * 12 + low.x], kSheenAlbedo[(low.y + 1) * 12 + low.x + 1], t.x);
	return lerp(a, b, t.y);
}

// what Shade lights: the Lambertian diffuse color, and the specular's Fresnel at normal incidence (f0) and at grazing
// angles (f90): the metallic-roughness model's mix of a dielectric (f0 from the ior, tinted by KHR_materials_specular's
// color, both scaled by its strength) and a metal (f0 the base color), or a specular-glossiness material's
struct Surface
{
	float3 diffuseColor;
	float3 f0;
	float f90;
	float roughness;
	float occlusion;
	float3 normal;
	// KHR_materials_clearcoat: a dielectric layer (f0 0.04) of a strength, roughness and normal over the rest, which
	// keeps what the layer's fresnel leaves; 0 for none
	float clearcoat;
	float clearcoatRoughness;
	float3 clearcoatNormal;
	// KHR_materials_sheen: its color (black for none) and roughness
	float3 sheenColor;
	float sheenRoughness;
};

// how much of what is below a clearcoat layer it reflects instead, seen from view (the spec's approximation, by the
// layer's normal)
float ClearcoatFresnel(Surface surface, float3 view)
{
	return surface.clearcoat * FresnelSchlick(float3(0.04), 1.0, max(dot(surface.clearcoatNormal, view), 1e-4)).r;
}

// the light a surface reflects towards view: from gLights, and the environment (by occlusion). linear, before exposure
float3 Shade(Surface surface, float3 view, float3 position)
{
	float3 normal = surface.normal;
	float roughness = surface.roughness;
	float alpha = max(roughness * roughness, 1e-3);
	float nDotV = max(dot(normal, view), 1e-4);

	float3 environmentSpecular = EnvironmentBrdfApprox(surface.f0, surface.f90, roughness, nDotV);
	EnvironmentData environment = gEnvironment[0];
	float3 reflected = reflect(-view, normal);
	float3 result = surface.occlusion * (surface.diffuseColor * EnvironmentIrradiance(environment, normal) +
										 environmentSpecular * EnvironmentRadiance(environment, reflected, roughness));

	// the sheen over that, which takes its albedo of it, and lit by the environment's light around the normal
	float sheenMax = max(surface.sheenColor.r, max(surface.sheenColor.g, surface.sheenColor.b));
	float sheenAlbedoView = SheenAlbedo(nDotV, surface.sheenRoughness);
	if (sheenMax > 0.0)
		result = result * (1.0 - sheenMax * sheenAlbedoView) +
				 surface.occlusion * surface.sheenColor * sheenAlbedoView * EnvironmentIrradiance(environment, normal);

	// and the clearcoat over all of it
	float coatFresnel = ClearcoatFresnel(surface, view);
	float coatNDotV = max(dot(surface.clearcoatNormal, view), 1e-4);
	float coatAlpha = max(surface.clearcoatRoughness * surface.clearcoatRoughness, 1e-3);
	if (surface.clearcoat > 0.0)
		result = result * (1.0 - coatFresnel) +
				 surface.occlusion * surface.clearcoat * EnvironmentBrdfApprox(float3(0.04), 1.0, surface.clearcoatRoughness, coatNDotV) *
					 EnvironmentRadiance(environment, reflect(-view, surface.clearcoatNormal), surface.clearcoatRoughness);

	for (uint lightIt = 0; lightIt < gPushConstants.lightCount; lightIt++)
	{
		LightData light = gLights[lightIt];

		float3 toLight;
		float attenuation = 1.0;
		if (light.type == LIGHT_TYPE_DIRECTIONAL)
		{
			toLight = -light.direction.xyz;
		}
		else
		{
			// inverse square, windowed to 0 at the range (KHR_lights_punctual's recommendation)
			float3 offset = light.positionRange.xyz - position;
			float distance2 = max(dot(offset, offset), 1e-8);
			toLight = offset * rsqrt(distance2);
			attenuation = 1.0 / distance2;
			if (light.positionRange.w > 0.0)
			{
				float ratio = sqrt(distance2) / light.positionRange.w;
				float window = saturate(1.0 - ratio * ratio * ratio * ratio);
				attenuation *= window * window;
			}
			if (light.type == LIGHT_TYPE_SPOT)
			{
				float cone = saturate(dot(light.direction.xyz, -toLight) * light.spotScale + light.spotOffset);
				attenuation *= cone * cone;
			}
		}

		if (attenuation <= 0.0)
			continue;

		float3 halfway = normalize(toLight + view);
		float vDotH = saturate(dot(view, halfway));
		float3 lit = 0.0;
		float nDotL = dot(normal, toLight);
		if (nDotL > 0.0)
		{
			float nDotH = saturate(dot(normal, halfway));
			float3 fresnel = FresnelSchlick(surface.f0, surface.f90, vDotH);
			float3 diffuse = (1.0 - fresnel) * surface.diffuseColor / kPi;
			float3 specular = fresnel * DistributionGgx(nDotH, alpha) * VisibilitySmithGgxCorrelated(nDotL, nDotV, alpha);
			lit = (diffuse + specular) * nDotL;
			if (sheenMax > 0.0)
			{
				float scaling = min(1.0 - sheenMax * sheenAlbedoView, 1.0 - sheenMax * SheenAlbedo(nDotL, surface.sheenRoughness));
				lit = lit * scaling + surface.sheenColor * DistributionCharlie(nDotH, surface.sheenRoughness) *
										  VisibilitySheen(nDotL, nDotV, surface.sheenRoughness) * nDotL;
			}
		}
		float coatNDotL = dot(surface.clearcoatNormal, toLight);
		if (surface.clearcoat > 0.0)
		{
			lit *= 1.0 - coatFresnel;
			if (coatNDotL > 0.0)
				lit += surface.clearcoat * FresnelSchlick(float3(0.04), 1.0, vDotH) *
					   DistributionGgx(saturate(dot(surface.clearcoatNormal, halfway)), coatAlpha) *
					   VisibilitySmithGgxCorrelated(coatNDotL, coatNDotV, coatAlpha) * coatNDotL;
		}
		result += lit * light.intensity.rgb * attenuation;
	}

	return result;
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
	// z is reconstructed from the stored (unit length) normal, then x and y are scaled, as gltf defines
	// normalTexture.scale: normalize(normal * (scale, scale, 1))
	float3 tangentSpaceNormal = float3(xy * scale, sqrt(saturate(1.0 - dot(xy, xy))));
	float3 alongU = tangent.xyz - normal * dot(normal, tangent.xyz);
	if (abs(tangent.w) > 0.5 && dot(alongU, alongU) > 1e-4 * dot(tangent.xyz, tangent.xyz))
		return faceSign * normalize(mul(tangentSpaceNormal, TangentFrame(normal, normalize(alongU), tangent.w, normalView)));
	return normalize(mul(tangentSpaceNormal, CotangentFrame(faceSign * normal, position, uv)));
}

// a fragment's color and alpha, lit and with emissive added
float4 ShadeFragment(VertexOutput input, bool isFrontFace)
{
	VertexP3fN3fTa4fT014fC4f v = input.vertex;

	uint viewAndMaterialId = gPushConstants.viewAndMaterialId;

	uint materialId = viewAndMaterialId & (SHADER_TYPES_MATERIAL_COUNT - 1u);

	MaterialData material = gMaterialData[materialId];

	// linear: the vertex colors hold the materials' diffuse colors, and textures are sampled through srgb formats
	float4 color = v.color * material.color;

	// cutouts (foliage, fences) keep their mask in the diffuse texture's alpha channel, or in a separate texture
	if ((material.flags & MATERIAL_FLAG_TEXTURE) != 0u)
	{
		float4 sample = SampleView(material.baseColorView, v.texCoord01);
		if (sample.a < material.alphaCutoff)
			discard;

		color *= sample;
	}

	if ((material.flags & MATERIAL_FLAG_ALPHA_TEXTURE) != 0u && SampleView(material.alphaView, v.texCoord01).r < material.alphaCutoff)
		discard;

	// a back face (only drawn for double sided materials) is lit as seen from behind, as gltf defines it: its normal,
	// normal mapped or not, is reversed
	float faceSign = isFrontFace ? 1.0 : -1.0;
	// lines and points without normals (zero ones, see mesh::Submesh::topology) are unlit: gltf draws them in their base
	// color plus emissive
	bool unlit = dot(v.normal, v.normal) == 0.0 || (material.flags & MATERIAL_FLAG_UNLIT) != 0u;
	float3 normal = unlit ? float3(0.0, 0.0, 1.0) : normalize(v.normal);
	float3 geometryNormal = faceSign * normal;
	if (!unlit && (material.flags & MATERIAL_FLAG_NORMAL_TEXTURE) != 0u)
		normal = NormalFromMap(material.normalView, material.normalScale, normal, v.tangent, v.position, v.texCoord01, faceSign);
	else
		normal = geometryNormal;

	// light the surface gives off, unaffected by the lighting
	float3 emissive = material.emissive.rgb;
	if ((material.flags & MATERIAL_FLAG_EMISSIVE_TEXTURE) != 0u)
		emissive *= SampleView(material.emissiveView, v.texCoord01).rgb;

	// KHR_materials_unlit: the base color alone. lines and points without normals: plus emissive
	if (unlit)
	{
		bool unlitMaterial = (material.flags & MATERIAL_FLAG_UNLIT) != 0u;
		return float4(color.rgb + (unlitMaterial ? float3(0.0) : emissive), color.a);
	}

	float metallic = material.metallic;
	float roughness = material.roughness;
	if ((material.flags & MATERIAL_FLAG_METALLIC_ROUGHNESS_TEXTURE) != 0u)
	{
		float2 sample = SampleView(material.metallicRoughnessView, v.texCoord01).rg;
		roughness *= sample.r;
		metallic *= sample.g;
	}

	// ambient occlusion only darkens the ambient light, the stand-in for indirect light, which is what gltf applies it to
	float occlusion = 1.0;
	if ((material.flags & MATERIAL_FLAG_OCCLUSION_TEXTURE) != 0u)
		occlusion = lerp(1.0, SampleView(material.occlusionView, v.texCoord01).r, material.emissive.w);

	// the specular (KHR_materials_specular and KHR_materials_ior, or specular-glossiness)
	float specular = material.specular;
	if ((material.flags & MATERIAL_FLAG_SPECULAR_TEXTURE) != 0u)
		specular *= SampleView(material.specularView, v.texCoord01).r;
	float4 specularColor = float4(material.specularColor.rgb, 1.0);
	if ((material.flags & MATERIAL_FLAG_SPECULAR_COLOR_TEXTURE) != 0u)
		specularColor *= SampleView(material.specularColorView, v.texCoord01);

	Surface surface;
	surface.occlusion = occlusion;
	surface.normal = normal;

	// KHR_materials_clearcoat: its own normal map, else the geometry's normal (not the base's normal map)
	surface.clearcoat = 0.0;
	surface.clearcoatRoughness = 0.0;
	surface.clearcoatNormal = geometryNormal;
	if ((material.flags & MATERIAL_FLAG_CLEARCOAT) != 0u)
	{
		surface.clearcoat = material.clearcoat.x;
		if ((material.flags & MATERIAL_FLAG_CLEARCOAT_TEXTURE) != 0u)
			surface.clearcoat *= SampleView(material.clearcoatView, v.texCoord01).r;
		surface.clearcoatRoughness = material.clearcoat.y;
		if ((material.flags & MATERIAL_FLAG_CLEARCOAT_ROUGHNESS_TEXTURE) != 0u)
			surface.clearcoatRoughness *= SampleView(material.clearcoatRoughnessView, v.texCoord01).r;
		if ((material.flags & MATERIAL_FLAG_CLEARCOAT_NORMAL_TEXTURE) != 0u)
			surface.clearcoatNormal = NormalFromMap(
				material.clearcoatNormalView, material.clearcoat.z, normalize(v.normal), v.tangent, v.position, v.texCoord01, faceSign);
	}

	// KHR_materials_sheen
	surface.sheenColor = 0.0;
	surface.sheenRoughness = 0.0;
	if ((material.flags & MATERIAL_FLAG_SHEEN) != 0u)
	{
		surface.sheenColor = material.sheen.rgb;
		if ((material.flags & MATERIAL_FLAG_SHEEN_COLOR_TEXTURE) != 0u)
			surface.sheenColor *= SampleView(material.sheenColorView, v.texCoord01).rgb;
		surface.sheenRoughness = material.sheen.a;
		if ((material.flags & MATERIAL_FLAG_SHEEN_ROUGHNESS_TEXTURE) != 0u)
			surface.sheenRoughness *= SampleView(material.sheenRoughnessView, v.texCoord01).r;
	}
	if ((material.flags & MATERIAL_FLAG_SPECULAR_GLOSSINESS) != 0u)
	{
		// the specular color is f0, and the diffuse keeps what it leaves
		surface.f0 = specularColor.rgb;
		surface.f90 = 1.0;
		surface.diffuseColor = color.rgb * (1.0 - max(surface.f0.r, max(surface.f0.g, surface.f0.b)));
		surface.roughness = 1.0 - saturate(material.roughness * specularColor.a);
	}
	else
	{
		float ior = material.specularColor.a;
		float reflectance = (ior - 1.0) / (ior + 1.0);
		float3 dielectricF0 = min(reflectance * reflectance * specularColor.rgb, 1.0) * specular;
		surface.f0 = lerp(dielectricF0, color.rgb, metallic);
		surface.f90 = lerp(specular, 1.0, metallic);
		surface.diffuseColor = lerp(color.rgb, float3(0.0), metallic);
		surface.roughness = roughness;
	}

	uint viewId = viewAndMaterialId >> SHADER_TYPES_MATERIAL_INDEX_BITS;
	float3 eye = gViewData[gPushConstants.frameIndex][viewId].eyePosition.xyz;
	float3 view = normalize(eye - v.position);

	// the clearcoat covers the emission too
	return float4(Shade(surface, view, v.position) + emissive * (1.0 - ClearcoatFresnel(surface, view)), color.a);
}

[shader("fragment")]
FragmentOutput FragmentMain(VertexOutput input, bool isFrontFace : SV_IsFrontFace)
{
	float4 color = ShadeFragment(input, isFrontFace);

	// alpha 1: the color target is cleared to alpha 0, so ComputeMain draws the backdrop where nothing opaque is
	FragmentOutput output;
	output.color = float4(color.rgb, 1.0);
	return output;
}

// a blended fragment: pushed on its pixel's list (see OitNode), for ComputeMain to sort and blend. the depth test must
// come first (it writes the lists, not the attachments), so it does: early fragment tests
[shader("fragment")]
[earlydepthstencil]
void FragmentTransparent(VertexOutput input, bool isFrontFace : SV_IsFrontFace)
{
	float4 color = ShadeFragment(input, isFrontFace);
	if (color.a <= 0.0)
		return;

	uint node;
	InterlockedAdd(gOitCounter[0], 1u, node);
	if (node >= gPushConstants.oitNodeCapacity)
		return;

	uint pixel = uint(input.hPos.y) * gPushConstants.framebufferWidth + uint(input.hPos.x);
	uint next;
	InterlockedExchange(gOitHeads[pixel], node, next);

	OitNode result;
	result.color = PackR11G11B10(color.rgb * saturate(color.a));
	result.alphaDepth = (uint(saturate(color.a) * 255.0 + 0.5) << 24) | uint(saturate(input.hPos.z) * 16777215.0 + 0.5);
	result.next = next;
	gOitNodes[node] = result;
}

// Khronos PBR Neutral: leaves colors up to about 0.76 nearly as they are (base colors render as themselves under a
// white light of 1), and compresses brighter ones towards white, desaturating them
float3 TonemapPbrNeutral(float3 color)
{
	const float startCompression = 0.8 - 0.04;
	const float desaturation = 0.15;

	float x = min(color.r, min(color.g, color.b));
	float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
	color -= offset;

	float peak = max(color.r, max(color.g, color.b));
	if (peak < startCompression)
		return color;

	const float d = 1.0 - startCompression;
	float newPeak = 1.0 - d * d / (peak + d - startCompression);
	color *= newPeak / peak;

	float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
	return lerp(color, newPeak * float3(1.0), g);
}

float3 ApplySRGBCurve(float3 x)
{
	// Approximately pow(x, 1.0 / 2.2)
	return select(x < 0.0031308, 12.92 * x, 1.055 * pow(x, 1.0 / 2.4) - 0.055);
}

float3 RemoveSRGBCurve(float3 x)
{
	// Approximately pow(x, 2.2)
	return select(x < 0.04045, x / 12.92, pow((x + 0.055) / 1.055, 2.4));
}

[shader("compute")]
[numthreads(16, 16, 1)]
void ComputeMain(
	uint3 groupId : SV_GroupID,
	uint3 groupThreadId : SV_GroupThreadID,
	uint3 dispatchThreadId : SV_DispatchThreadID,
	uint groupIndex : SV_GroupIndex)
{
	uint frameIndex = gPushConstants.frameIndex;

	Texture2D src = gTextures[SHADER_TYPES_RENDER_TARGET_TEXTURE_BASE + frameIndex];
	RWTexture2D dst = gRWTextures[frameIndex];

	uint2 dstDim;
	dst.GetDimensions(dstDim.x, dstDim.y);

	static const uint2 bucketSize = uint2(16, 16); 

	for (uint j = 0; j < bucketSize.x; j++)
	{
		uint y = dispatchThreadId.y * bucketSize.y + j;
		if (y >= dstDim.y)
			break;

		for (uint i = 0; i < bucketSize.y; i++)
		{
			uint x = dispatchThreadId.x * bucketSize.x + i;
			if (x >= dstDim.x)
				break;

			uint2 xy = uint2(x, y);

			// the render target is linear, the swapchain (a storage image, so not an srgb format) is not: exposed,
			// tonemapped, and then the srgb curve
			float4 color = src.Load(int3(xy, 0));

			// the environment where nothing opaque was drawn, in the view the pixel is in (outside them, e.g. where a view
			// is letterboxed, the clear color stays): along the ray through the pixel, between two depths of it
			if (color.a == 0.0 && gPushConstants.environmentBackdrop != 0u)
			{
				for (uint viewIt = 0; viewIt < gPushConstants.viewCount; viewIt++)
				{
					ViewData view = gViewData[frameIndex][viewIt];
					float2 local = (float2(xy) + 0.5 - view.viewport.xy) / view.viewport.zw;
					if (any(local < 0.0) || any(local >= 1.0))
						continue;
					float2 ndc = local * 2.0 - 1.0;
					float4 nearPoint = mul(view.inverseViewProjection, float4(ndc, 0.0, 1.0));
					float4 farPoint = mul(view.inverseViewProjection, float4(ndc, 0.5, 1.0));
					float3 ray = normalize(farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w);
					color.rgb = EnvironmentRadiance(gEnvironment[0], ray, 0.0);
					break;
				}
			}

			// the blended fragments over the rest: the pixel's list, its nearest layers sorted near to far (by insertion),
			// then blended far to near
			uint node = gOitHeads[y * gPushConstants.framebufferWidth + x];
			if (node != SHADER_TYPES_OIT_NONE)
			{
				uint layers[SHADER_TYPES_OIT_MAX_LAYERS];
				uint keys[SHADER_TYPES_OIT_MAX_LAYERS];
				uint layerCount = 0;
				for (uint visited = 0; node != SHADER_TYPES_OIT_NONE && node < gPushConstants.oitNodeCapacity && visited < 4096u; visited++)
				{
					OitNode current = gOitNodes[node];
					uint depth = current.alphaDepth & 0xffffffu;
					if (layerCount < SHADER_TYPES_OIT_MAX_LAYERS || depth < keys[layerCount - 1])
					{
						// past the last kept layer if there is room, else over it (the farthest drops out)
						uint slot = layerCount < SHADER_TYPES_OIT_MAX_LAYERS ? layerCount++ : layerCount - 1;
						while (slot > 0 && keys[slot - 1] > depth)
						{
							keys[slot] = keys[slot - 1];
							layers[slot] = layers[slot - 1];
							slot--;
						}
						keys[slot] = depth;
						layers[slot] = node;
					}
					node = current.next;
				}
				for (uint layerIt = layerCount; layerIt > 0; layerIt--)
				{
					OitNode layer = gOitNodes[layers[layerIt - 1]];
					float alpha = float(layer.alphaDepth >> 24) / 255.0;
					color.rgb = color.rgb * (1.0 - alpha) + UnpackR11G11B10(layer.color);
				}
			}

			float3 mapped = TonemapPbrNeutral(max(color.rgb, 0.0) * gPushConstants.exposure);
			dst.Store(xy, float4(ApplySRGBCurve(saturate(mapped)), 1.0));
		}
	}
}
