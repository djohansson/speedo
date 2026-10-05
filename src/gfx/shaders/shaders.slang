module speedo;

__include types;

#include "capi.h"

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_GLOBAL)]]
[[vk::push_constant]]
ConstantBuffer<PushConstants> gPushConstants;

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS)]]
StructuredBuffer<VertexP3fN3fT014fC4f> gVertexBuffer;

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

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<ModelInstance> gModelInstances;

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
	VertexP3fN3fT014fC4f vertex;
	float4 hPos : SV_Position;
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

    VertexP3fN3fT014fC4f v = gVertexBuffer[input.vertexId];
    ModelInstance i = gModelInstances[modelInstanceId];

	float3 worldPosition = mul(i.modelTransform, float4(v.position, 1.0)).xyz;
	
	output.hPos = mul(view.viewProjection, float4(worldPosition, 1.0));
	
	return output;
}

[shader("vertex")]
VertexOutput VertexMain(InputStream input)
{
	VertexOutput output;

	uint frameIndex = gPushConstants.frameIndex;
    uint viewAndMaterialId = gPushConstants.viewAndMaterialId;
    uint modelInstanceId = gPushConstants.modelInstanceId;

	uint viewId = viewAndMaterialId >> SHADER_TYPES_MATERIAL_INDEX_BITS;

	ViewData view = gViewData[frameIndex][viewId];

    VertexP3fN3fT014fC4f v = gVertexBuffer[input.vertexId];
    ModelInstance i = gModelInstances[modelInstanceId];

	float3 worldPosition = mul(i.modelTransform, float4(v.position, 1.0)).xyz;
	float3 worldNormal = mul(i.inverseTransposeModelTransform, float4(v.normal, 0.0)).xyz;

	output.hPos = mul(view.viewProjection, float4(worldPosition, 1.0));
	output.vertex.position = worldPosition;
	output.vertex.normal = worldNormal;
	output.vertex.texCoord01 = v.texCoord01;
	output.vertex.color = v.color;

	return output;
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

[shader("fragment")]
FragmentOutput FragmentMain(VertexOutput input)
{
	VertexP3fN3fT014fC4f v = input.vertex;
	FragmentOutput output;

	uint viewAndMaterialId = gPushConstants.viewAndMaterialId;

	uint materialId = viewAndMaterialId & (SHADER_TYPES_MATERIAL_COUNT - 1u);

	MaterialData material = gMaterialData[materialId];

	uint textureAndSamplerId = material.textureAndSamplerId;
	uint textureId = textureAndSamplerId >> SHADER_TYPES_GLOBAL_TEXTURE_INDEX_BITS;
	SamplerState sampler = gSamplers[textureAndSamplerId & (SHADER_TYPES_GLOBAL_SAMPLER_COUNT - 1u)];
	float2 uv = v.texCoord01.xy;

	// linear: the vertex colors hold the materials' diffuse colors, and textures are sampled through srgb formats
	float4 color = v.color * material.color;

	// cutouts (foliage, fences) keep their mask in the diffuse texture's alpha channel, or in a separate texture
	if ((material.flags & MATERIAL_FLAG_TEXTURE) != 0u)
	{
		float4 sample = gTextures[textureId].Sample(sampler, uv);
		if (sample.a < material.alphaCutoff)
			discard;

		color *= sample;
	}

	if ((material.flags & MATERIAL_FLAG_ALPHA_TEXTURE) != 0u && gTextures[material.alphaTextureId].Sample(sampler, uv).r < material.alphaCutoff)
		discard;

	float3 normal = normalize(v.normal);
	if ((material.flags & MATERIAL_FLAG_NORMAL_TEXTURE) != 0u)
	{
		float2 xy = gTextures[material.normalTextureId].Sample(sampler, uv).rg * 2.0 - 1.0;
		// z is reconstructed from the stored (unit length) normal, then x and y are scaled, as gltf defines
		// normalTexture.scale: normalize(normal * (scale, scale, 1))
		float3 tangentSpaceNormal = float3(xy * material.normalScale, sqrt(saturate(1.0 - dot(xy, xy))));
		normal = normalize(mul(tangentSpaceNormal, CotangentFrame(normal, v.position, uv)));
	}

	// a fixed light from above, and some ambient, so that the shapes read
	const float3 lightDirection = normalize(float3(0.3, 1.0, 0.5));
	float diffuse = saturate(dot(normal, lightDirection));

	// light the surface gives off, unaffected by the lighting
	float3 emissive = material.emissive.rgb;
	if ((material.flags & MATERIAL_FLAG_EMISSIVE_TEXTURE) != 0u)
		emissive *= gTextures[material.emissiveTextureId].Sample(sampler, uv).rgb;

	output.color = float4(color.rgb * (0.3 + 0.7 * diffuse) + emissive, color.a);

	return output;
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

            // the render target is linear, the swapchain (a storage image, so not an srgb format) is not
            float4 color = src.Load(int3(xy, 0));
            dst.Store(xy, float4(ApplySRGBCurve(saturate(color.rgb)), color.a));
        }
    }

	// float4 color[3];
	// color[0].r = (r0 >> 0  & 0xFF) / 255.0f;
	// color[0].g = (r0 >> 8  & 0xFF) / 255.0f;
	// color[0].b = (r0 >> 16 & 0xFF) / 255.0f;
	// color[0].a = (r0 >> 24 & 0xFF) / 255.0f;

	// color[1].r = (r1 >> 0  & 0xFF) / 255.0f;
	// color[1].g = (r1 >> 8  & 0xFF) / 255.0f;
	// color[1].b = (r1 >> 16 & 0xFF) / 255.0f;
	// color[1].a = (r1 >> 24 & 0xFF) / 255.0f;

	// color[2].r = (r2 >> 0  & 0xFF) / 255.0f;
	// color[2].g = (r2 >> 8  & 0xFF) / 255.0f;
	// color[2].b = (r2 >> 16 & 0xFF) / 255.0f;
	// color[2].a = (r2 >> 24 & 0xFF) / 255.0f;

	// float4 result = lerp(lerp(lerp(0, color[2], color[2].a), color[1], color[1].a), color[0], color[0].a);
	// result.a = 1.0f;
	// colorTarget[threadIds.xy] = result;
}

