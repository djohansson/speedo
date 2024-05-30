module speedo;

__include types;

#include "capi.h"

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_GLOBAL)]]
[[vk::push_constant]]
ConstantBuffer<PushConstants> g_pushConstants;

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS)]]
StructuredBuffer<VertexP3fN3fT014fC4f> g_vertexBuffer;

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES)]]
Texture2D g_textures[SHADER_TYPES_GLOBAL_TEXTURE_COUNT];

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES)]]
RWTexture2D g_rwtextures[SHADER_TYPES_GLOBAL_RW_TEXTURE_COUNT];

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS)]]
SamplerState g_samplers[SHADER_TYPES_GLOBAL_SAMPLER_COUNT];

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_VIEW)]]
StructuredBuffer<ViewData> g_viewData[SHADER_TYPES_FRAME_COUNT];

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_MATERIAL)]]
StructuredBuffer<MaterialData> g_materialData;

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<ModelInstance> g_modelInstances;

struct InputStream : IInputStream
{
	uint vertexId : SV_VertexID;
	uint instanceId : SV_InstanceID;
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

float3 signedOctEncode(float3 n)
{
	float3 OutN;

	n /= (abs(n.x) + abs(n.y) + abs(n.z));

	OutN.y = n.y * 0.5 + 0.5;
	OutN.x = n.x * 0.5 + OutN.y;
	OutN.y = n.x * -0.5 + OutN.y;
	OutN.z = saturate(n.z*FLT_MAX);

	return OutN;
}

float3 signedOctDecode(float3 n)
{
	float3 OutN;

	OutN.x = (n.x - n.y);
	OutN.y = (n.x + n.y) - 1.0;
	OutN.z = n.z * 2.0 - 1.0;
	OutN.z = OutN.z * (1.0 - abs(OutN.x) - abs(OutN.y));
 
	return normalize(OutN);
}

VertexOutput vertexMain(InputStream input)
{
	VertexOutput output;

	uint frameIndex = g_pushConstants.frameIndex;
	uint viewAndMaterialId = g_pushConstants.viewAndMaterialId;

	uint viewId = viewAndMaterialId >> SHADER_TYPES_MATERIAL_INDEX_BITS;

	ViewData view = g_viewData[frameIndex][viewId];

	VertexP3fN3fT014fC4f v = g_vertexBuffer[input.vertexId];
	ModelInstance i = g_modelInstances[input.instanceId];

	float3 worldPosition = mul(i.modelTransform, float4(v.position, 1.0)).xyz;
	float3 worldNormal = mul(i.inverseTransposeModelTransform, float4(v.normal, 0.0)).xyz;

	output.hPos = mul(view.viewProjection, float4(worldPosition, 1.0));
	output.vertex.position = worldPosition;
	output.vertex.normal = worldNormal;
	output.vertex.texCoord01 = v.texCoord01;
	output.vertex.color = v.color;

	return output;
}

FragmentOutput fragmentMain(VertexOutput input)
{
	VertexP3fN3fT014fC4f v = input.vertex;
	FragmentOutput output;

	uint viewAndMaterialId = g_pushConstants.viewAndMaterialId;

	uint materialId = viewAndMaterialId & (SHADER_TYPES_MATERIAL_COUNT - 1u);

	MaterialData material = g_materialData[materialId];

	uint textureAndSamplerId = material.textureAndSamplerId;

	uint textureId = textureAndSamplerId >> SHADER_TYPES_GLOBAL_TEXTURE_INDEX_BITS;
	uint samplerId = textureAndSamplerId & (SHADER_TYPES_GLOBAL_SAMPLER_COUNT - 1u);

	Texture2D texture = g_textures[textureId];
	SamplerState sampler = g_samplers[samplerId];

	const uint checkerScale = 2;
	const float checkerScaleInv = 1.f / checkerScale;
	float checkers = checkerScale * frac(checkerScaleInv * (floor(v.position.x) + floor(v.position.y) + floor(v.position.z)));

	float4 sample = texture.Sample(sampler, v.texCoord01.xy);

	output.color = 
		lerp(
			lerp(
				lerp(
					lerp(v.color, material.color, 0.5),
					float4(float3(checkers), 1.0),
					0.2),
				float4(v.normal, 1.0),
				0.2),
			sample,
			sample.a);

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

