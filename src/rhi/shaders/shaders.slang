#include "shadertypes.h"

[[vk::binding(0, DescriptorSetCategory_Global)]]
[[vk::push_constant]]
ConstantBuffer<PushConstants> g_pushConstants;

[[vk::binding(0, DescriptorSetCategory_GlobalTextures)]]
Texture2D g_textures[ShaderTypes_GlobalTextureCount];

[[vk::binding(0, DescriptorSetCategory_GlobalRWTextures)]]
RWTexture2D g_rwtextures[ShaderTypes_GlobalRWTextureCount];

[[vk::binding(0, DescriptorSetCategory_GlobalSamplers)]]
SamplerState g_samplers[ShaderTypes_GlobalSamplerCount];

[[vk::binding(0, DescriptorSetCategory_View)]]
StructuredBuffer<ViewData> g_viewData[ShaderTypes_FrameCount];

[[vk::binding(0, DescriptorSetCategory_Material)]]
StructuredBuffer<MaterialData> g_materialData;

[[vk::binding(0, DescriptorSetCategory_Object)]]
StructuredBuffer<ObjectData> g_objectData[ShaderTypes_ObjectSetCount];

struct InputStream
{
	float3 position : POSITION;
	float3 normal : NORMAL;
	float2 texCoord : TEXCOORD0;
	float3 color : COLOR;
	uint vertexId : SV_VertexID;
};

struct Interpolants
{
	float3 position;
	float3 normal;
	float2 texCoord;
	float3 color;
};

struct VSOutput
{
	float4 hPos : SV_Position;
	Interpolants data : Interpolants;
};

struct FSOutput
{
	float4 color : SV_Target;
};

VSOutput vertexMain(InputStream input)
{
	VSOutput output = (VSOutput)0;

	uint frameIndex = g_pushConstants.frameIndex;
	uint viewAndMaterialId = g_pushConstants.viewAndMaterialId;

	uint viewId = viewAndMaterialId >> ShaderTypes_MaterialIndexBits;

	ViewData view = g_viewData[frameIndex][viewId];

	uint objectId = g_pushConstants.objectId;

	uint objectSetIndex = objectId >> ShaderTypes_ObjectIndexBits;
	uint objectIndex = objectId & (ShaderTypes_ObjectCount - 1u);

	ObjectData object = g_objectData[objectSetIndex][objectIndex];

	float3 worldPosition = mul(object.modelTransform, float4(input.position, 1.0)).xyz;
	float3 worldNormal = mul(object.inverseTransposeModelTransform, float4(input.normal, 0.0)).xyz;

	output.hPos = mul(view.viewProjectionTransform, float4(worldPosition, 1.0));
	output.data.position = worldPosition;
	output.data.normal = worldNormal;
	output.data.texCoord = input.texCoord;
	output.data.color = input.color;

	return output;
}

FSOutput fragmentMain(Interpolants input : Interpolants)
{
	FSOutput output = (FSOutput)0;

	uint viewAndMaterialId = g_pushConstants.viewAndMaterialId;

	uint materialId = viewAndMaterialId & (ShaderTypes_MaterialCount - 1u);

	MaterialData material = g_materialData[materialId];

	uint textureAndSamplerId = material.textureAndSamplerId;

	uint textureId = textureAndSamplerId >> ShaderTypes_GlobalTextureIndexBits;
	uint samplerId = textureAndSamplerId & (ShaderTypes_GlobalSamplerCount - 1u);

	Texture2D texture = g_textures[textureId];
	SamplerState sampler = g_samplers[samplerId];

	const uint checkerScale = 2;
	const float checkerScaleInv = 1.f / checkerScale;
	float checkers = checkerScale * frac(checkerScaleInv * (floor(input.position.x) + floor(input.position.y) + floor(input.position.z)));

	output.color += 0.2 * float4(float3(checkers), 1.0);
	output.color += 0.2 * float4(input.normal, 1.0);
	output.color += 0.2 * float4(input.color, 1.0);
	output.color += 0.2 * texture.Sample(sampler, input.texCoord);
	output.color += 0.2 * material.color;

	return output;
}

[numthreads(8, 8, 1)]
void computeMain(
	uint3 groupId : SV_GroupID,
	uint3 groupThreadId : SV_GroupThreadID,
	uint3 dispatchThreadId : SV_DispatchThreadID,
	uint groupIndex : SV_GroupIndex)
{
	RWTexture2D<float4> colorSource = g_rwtextures[0];
	colorSource[dispatchThreadId.xy] = float4(1.0f, 0.0f, 0.0f, 1.0f);

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

