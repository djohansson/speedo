// the vertex stage: morph targets, skinning, the vertex interfaces, and VertexZPrepass and VertexMain. a part of shaders.slang, which includes it after capi.h and sky.h

// a vertex moved by its submesh's morph targets: each target's deltas (see MorphDelta) times the target's weight
void Morph(DrawRecord draw, uint vertexId, uint frameIndex, inout float3 position, inout float3 normal, inout float3 tangent)
{
	uint count = draw.morphTargetCount;
	uint row = draw.morphDeltaBase + (vertexId - draw.morphFirstVertex) * count;
	for (uint t = 0; t < count; t++)
	{
		float weight = gMorphWeights[frameIndex][draw.morphWeightBase + t];
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
	uint drawIndex : SV_DrawIndex; // in an indirect draw, else 0
};

struct VertexZPrepassOutput : IInterpolants
{
	float4 hPos : SV_Position;
};

struct VertexOutput : IInterpolants
{
	VertexP3fN3fTa4fT014fC4f vertex;
	float4 hPos : SV_Position;
	// the instance transform's scale (the cube root of its volume scale), for KHR_materials_volume's thickness
	float modelScale;
	// the draw's material (see DrawRecord), for the fragment stage
	nointerpolation uint materialId;
};

// VertexOutput, and what only the vertex stage writes (a system value the fragment stage can't take as input)
struct VertexMainOutput
{
	VertexOutput varyings;
	// points are a pixel (vulkan needs the size written for point lists)
	float pointSize : SV_PointSize;
};

[shader("vertex")]
VertexZPrepassOutput VertexZPrepass(InputStream input)
{
	VertexZPrepassOutput output;

	uint frameIndex = gPushConstants.frameIndex;
	DrawRecord draw = gDrawRecords[frameIndex][gDrawSlots[frameIndex][gPushConstants.drawSlot + input.drawIndex]];

	ViewData view = gViewData[frameIndex][gPushConstants.viewId];

	VertexP3fN3fTa4fT014fC4f v = gVertexBuffer[input.vertexId];
	ModelInstance i = gModelInstances[frameIndex][draw.modelInstanceId + input.instanceId];

	float3 position = v.position;
	if (draw.morphTargetCount > 0)
	{
		float3 normal = v.normal;
		float3 tangent = v.tangent.xyz;
		Morph(draw, input.vertexId, frameIndex, position, normal, tangent);
	}
	if (draw.jointBase != SHADER_TYPES_NOT_SKINNED)
		position = mul(SkinMatrix(input.vertexId, frameIndex, draw.jointBase), float4(position, 1.0)).xyz;
	float3 worldPosition = mul(i.modelTransform, float4(position, 1.0)).xyz;
	
	output.hPos = mul(view.viewProjection, float4(worldPosition, 1.0));
	
	return output;
}

[shader("vertex")]
VertexMainOutput VertexMain(InputStream input)
{
	VertexOutput output;

	uint frameIndex = gPushConstants.frameIndex;
	DrawRecord draw = gDrawRecords[frameIndex][gDrawSlots[frameIndex][gPushConstants.drawSlot + input.drawIndex]];

	ViewData view = gViewData[frameIndex][gPushConstants.viewId];

	VertexP3fN3fTa4fT014fC4f v = gVertexBuffer[input.vertexId];
	ModelInstance i = gModelInstances[frameIndex][draw.modelInstanceId + input.instanceId];

	// morph targets move the vertices first (in the space they are stored in), then skinned vertices move with their joints (in world space), before the instance's transform (the identity for them).
	// the normals and tangents go by the same matrix, which is exact for joints without non-uniform scale.
	float3 position = v.position;
	float3 normal = v.normal;
	float3 tangent = v.tangent.xyz;
	if (draw.morphTargetCount > 0)
		Morph(draw, input.vertexId, frameIndex, position, normal, tangent);
	if (draw.jointBase != SHADER_TYPES_NOT_SKINNED)
	{
		float4x4 skin = SkinMatrix(input.vertexId, frameIndex, draw.jointBase);
		position = mul(skin, float4(position, 1.0)).xyz;
		normal = mul(skin, float4(normal, 0.0)).xyz;
		tangent = mul(skin, float4(tangent, 0.0)).xyz;
	}

	float3 worldPosition = mul(i.modelTransform, float4(position, 1.0)).xyz;
	float3 worldNormal = mul(i.inverseTransposeModelTransform, float4(normal, 0.0)).xyz;

	// the shadow pass draws from its shadow views instead
	float4x4 viewProjection = gPushConstants.shadowView != SHADER_TYPES_NO_SHADOW
		? gShadowViews[frameIndex][gPushConstants.shadowView].viewProjection
		: view.viewProjection;
	output.hPos = mul(viewProjection, float4(worldPosition, 1.0));
	output.vertex.position = worldPosition;
	output.vertex.normal = worldNormal;
	// a mirroring transform flips the handedness of the tangent frame
	float3x3 linear = float3x3(i.modelTransform[0].xyz, i.modelTransform[1].xyz, i.modelTransform[2].xyz);
	output.vertex.tangent = float4(mul(linear, tangent), determinant(linear) < 0.0 ? -v.tangent.w : v.tangent.w);
	output.vertex.texCoord01 = v.texCoord01;
	output.vertex.color = v.color;
	output.modelScale = pow(abs(determinant(linear)), 1.0 / 3.0);
	output.materialId = draw.materialId;

	return { output, 1.0 };
}
