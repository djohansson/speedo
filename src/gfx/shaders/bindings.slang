// the resources the main layout binds: push constants, the layer groups' specialization constant, buffers, textures and samplers. a part of shaders.slang, which includes it after capi.h and sky.h

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_GLOBAL)]]
[[vk::push_constant]]
ConstantBuffer<PushConstants> gPushConstants;

// the layer groups (SHADER_TYPES_LAYERS_*) this pipeline is specialized without: their code compiles away. 0 in the
// generic pipelines, which draw any material
[vk::constant_id(0)]
const uint kSkippedLayers = 0;

// whether a material's layer can be drawn by this pipeline
bool Layer(MaterialData material, uint flag)
{
	return (kSkippedLayers & SHADER_TYPES_LAYERS_EXTENDED) == 0u && (material.flags & flag) != 0u;
}

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

// auto exposure's histograms, SHADER_TYPES_EXPOSURE_BINS per frame index
[[vk::binding(6, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS)]]
RWStructuredBuffer<uint> gExposureHistogram;

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

// the shadows' views, and each light's first (see ShadowData)
[[vk::binding(5, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<ShadowData> gShadowViews[SHADER_TYPES_FRAME_COUNT];
[[vk::binding(6, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<uint> gLightShadows[SHADER_TYPES_FRAME_COUNT];

// clustered lighting (see SHADER_TYPES_CLUSTER_COUNT): per view its clusters' slicing, per cluster its range of light
// indices, and the indices
[[vk::binding(7, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<ClusterView> gClusterViews[SHADER_TYPES_FRAME_COUNT];
[[vk::binding(8, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<uint2> gClusters[SHADER_TYPES_FRAME_COUNT];
[[vk::binding(9, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<uint> gClusterLights[SHADER_TYPES_FRAME_COUNT];

// the ui's frosted panels (see UiBlurRect)
[[vk::binding(10, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<UiBlurRect> gUiBlurRects[SHADER_TYPES_FRAME_COUNT];

// the frame's draws' per draw values (see DrawRecord)
[[vk::binding(11, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<DrawRecord> gDrawRecords[SHADER_TYPES_FRAME_COUNT];
