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

struct FragmentOutput
{
	float4 color : SV_Target;
};

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

// the light around everything, a uniform environment of this radiance: the stand-in for indirect light (there is no
// image based lighting). a white matte surface reflects as much of it
static const float kAmbientRadiance = 0.3;

// the glTF metallic-roughness brdf (the spec's appendix B): GGX distribution, height correlated Smith visibility, and
// Schlick's Fresnel, mixing a Lambertian diffuse and the specular by the Fresnel term
float3 FresnelSchlick(float3 f0, float cosine)
{
	return f0 + (1.0 - f0) * pow(saturate(1.0 - cosine), 5.0);
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
float3 EnvironmentBrdfApprox(float3 f0, float roughness, float nDotV)
{
	const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
	const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
	float4 r = roughness * c0 + c1;
	float a004 = min(r.x * r.x, exp2(-9.28 * nDotV)) * r.x + r.y;
	float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
	return f0 * ab.x + ab.y;
}

// the fresnel term of a mix of dielectric (f0 0.04, scaled by specular: KHR_materials_specular's specularWeight) and
// metal (f0 the base color)
float3 Fresnel(float3 baseColor, float metallic, float specular, float cosine)
{
	return lerp(specular * FresnelSchlick(float3(0.04), cosine), FresnelSchlick(baseColor, cosine), metallic);
}

// the light a surface reflects towards view: from gLights, and the ambient (by occlusion). linear, before exposure
float3 Shade(float3 baseColor, float metallic, float roughness, float specular, float occlusion, float3 normal, float3 view, float3 position)
{
	float3 diffuseColor = lerp(baseColor, float3(0.0), metallic);
	float alpha = max(roughness * roughness, 1e-3);
	float nDotV = max(dot(normal, view), 1e-4);

	float3 environmentSpecular = lerp(
		specular * EnvironmentBrdfApprox(float3(0.04), roughness, nDotV), EnvironmentBrdfApprox(baseColor, roughness, nDotV), metallic);
	float3 result = kAmbientRadiance * occlusion * (diffuseColor + environmentSpecular);

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

		float nDotL = dot(normal, toLight);
		if (nDotL <= 0.0 || attenuation <= 0.0)
			continue;

		float3 halfway = normalize(toLight + view);
		float nDotH = saturate(dot(normal, halfway));
		float3 fresnel = Fresnel(baseColor, metallic, specular, saturate(dot(view, halfway)));
		float3 diffuse = (1.0 - fresnel) * diffuseColor / kPi;
		float3 specular = fresnel * DistributionGgx(nDotH, alpha) * VisibilitySmithGgxCorrelated(nDotL, nDotV, alpha);
		result += (diffuse + specular) * light.intensity.rgb * attenuation * nDotL;
	}

	return result;
}

[shader("fragment")]
FragmentOutput FragmentMain(VertexOutput input, bool isFrontFace : SV_IsFrontFace)
{
	VertexP3fN3fTa4fT014fC4f v = input.vertex;
	FragmentOutput output;

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
	if (!unlit && (material.flags & MATERIAL_FLAG_NORMAL_TEXTURE) != 0u)
	{
		// the tangent frame follows the normal map's own (transformed) texcoords
		TextureView normalView = gTextureViews[material.normalView];
		float2 uv = ViewTexCoord(normalView, v.texCoord01);
		float2 xy = SampleView(normalView, uv).rg * 2.0 - 1.0;
		// z is reconstructed from the stored (unit length) normal, then x and y are scaled, as gltf defines
		// normalTexture.scale: normalize(normal * (scale, scale, 1))
		float3 tangentSpaceNormal = float3(xy * material.normalScale, sqrt(saturate(1.0 - dot(xy, xy))));
		// the vertex tangents if the mesh has them (interpolated w stays away from 0) and they aren't (nearly) parallel
		// to the normal, which some files have, else a derived frame
		float3 alongU = v.tangent.xyz - normal * dot(normal, v.tangent.xyz);
		if (abs(v.tangent.w) > 0.5 && dot(alongU, alongU) > 1e-4 * dot(v.tangent.xyz, v.tangent.xyz))
			normal = faceSign * normalize(mul(tangentSpaceNormal, TangentFrame(normal, normalize(alongU), v.tangent.w, normalView)));
		else
			normal = normalize(mul(tangentSpaceNormal, CotangentFrame(faceSign * normal, v.position, uv)));
	}
	else
	{
		normal *= faceSign;
	}

	// light the surface gives off, unaffected by the lighting
	float3 emissive = material.emissive.rgb;
	if ((material.flags & MATERIAL_FLAG_EMISSIVE_TEXTURE) != 0u)
		emissive *= SampleView(material.emissiveView, v.texCoord01).rgb;

	// KHR_materials_unlit: the base color alone. lines and points without normals: plus emissive
	if (unlit)
	{
		bool unlitMaterial = (material.flags & MATERIAL_FLAG_UNLIT) != 0u;
		output.color = float4(color.rgb + (unlitMaterial ? float3(0.0) : emissive), color.a);
		return output;
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

	uint viewId = viewAndMaterialId >> SHADER_TYPES_MATERIAL_INDEX_BITS;
	float3 eye = gViewData[gPushConstants.frameIndex][viewId].eyePosition.xyz;
	float3 view = normalize(eye - v.position);

	output.color = float4(Shade(color.rgb, metallic, roughness, material.specular, occlusion, normal, view, v.position) + emissive, color.a);

	return output;
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

	Texture2D src = gTextures[frameIndex];
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
			float3 mapped = TonemapPbrNeutral(max(color.rgb, 0.0) * gPushConstants.exposure);
			dst.Store(xy, float4(ApplySRGBCurve(saturate(mapped)), color.a));
		}
	}
}
