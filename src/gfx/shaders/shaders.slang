module speedo;

__include types;

#include "capi.h"

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_GLOBAL)]]
[[vk::push_constant]]
ConstantBuffer<PushConstants> gPushConstants;

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS)]]
StructuredBuffer<VertexP3fN3fTa4fT014fC4f> gVertexBuffer;

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
    ModelInstance i = gModelInstances[modelInstanceId];

	float3 worldPosition = mul(i.modelTransform, float4(v.position, 1.0)).xyz;
	
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
    ModelInstance i = gModelInstances[modelInstanceId];

	float3 worldPosition = mul(i.modelTransform, float4(v.position, 1.0)).xyz;
	float3 worldNormal = mul(i.inverseTransposeModelTransform, float4(v.normal, 0.0)).xyz;

	output.hPos = mul(view.viewProjection, float4(worldPosition, 1.0));
	output.vertex.position = worldPosition;
	output.vertex.normal = worldNormal;
	// a mirroring transform flips the handedness of the tangent frame
	float3x3 linear = float3x3(i.modelTransform[0].xyz, i.modelTransform[1].xyz, i.modelTransform[2].xyz);
	output.vertex.tangent = float4(mul(linear, v.tangent.xyz), determinant(linear) < 0.0 ? -v.tangent.w : v.tangent.w);
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
	bool unlit = dot(v.normal, v.normal) == 0.0;
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

	// a fixed light from above, and some ambient, so that the shapes read
	const float3 lightDirection = normalize(float3(0.3, 1.0, 0.5));
	float diffuse = saturate(dot(normal, lightDirection));

	// light the surface gives off, unaffected by the lighting
	float3 emissive = material.emissive.rgb;
	if ((material.flags & MATERIAL_FLAG_EMISSIVE_TEXTURE) != 0u)
		emissive *= SampleView(material.emissiveView, v.texCoord01).rgb;

	// ambient occlusion only darkens the ambient light, the stand-in for indirect light, which is what gltf applies it to
	float occlusion = 1.0;
	if ((material.flags & MATERIAL_FLAG_OCCLUSION_TEXTURE) != 0u)
		occlusion = lerp(1.0, SampleView(material.occlusionView, v.texCoord01).r, material.emissive.w);

	float lighting = unlit ? 1.0 : 0.3 * occlusion + 0.7 * diffuse;
	output.color = float4(color.rgb * lighting + emissive, color.a);

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

