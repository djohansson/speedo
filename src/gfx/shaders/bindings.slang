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

// the frames' histograms, SHADER_TYPES_HISTOGRAM_BINS per frame index: auto exposure's bins, then the distances' (see
// SHADER_TYPES_DISTANCE_BINS)
[[vk::binding(6, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS)]]
RWStructuredBuffer<uint> gExposureHistogram;

// the model's indices (the index buffer's), for what reads its triangles in compute (the indirect lighting's voxelizer)
[[vk::binding(7, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS)]]
StructuredBuffer<uint> gIndexBuffer;

// the model's BVH over what doesn't move (see gfx/bvh.h, bvh.slang): CWBVH nodes and triangles, for the indirect
// lighting's rays (GiData::trace)
[[vk::binding(8, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS)]]
StructuredBuffer<float4> gBvhNodes;
[[vk::binding(9, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS)]]
StructuredBuffer<float4> gBvhTriangles;

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES)]]
Texture2D gTextures[SHADER_TYPES_GLOBAL_TEXTURE_COUNT];
// the main depth's stencil: its first bit marks what moves (see CommandEncoder::SetStencilReference)
[[vk::binding(1, DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES)]]
Texture2D<uint> gMainStencil;
// the indirect lighting's voxels' resolved probes (see ComputeGiResolve), the clips stacked along z: the irradiance
// along the axes times the weight of the probes seen (per color its faces +x, -x, +y and -y, then red's and green's +z
// and -z, then blue's and the weight: see GiSample). sampled trilinearly, and
// written as storage images (gGiVolumeImages), always in the general layout
[[vk::binding(2, DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES)]]
Texture3D<float4> gGiVolumes[SHADER_TYPES_GI_VOLUME_COUNT];

[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES)]]
RWTexture2D gRWTextures[SHADER_TYPES_GLOBAL_RW_TEXTURE_COUNT];
// the depth pyramid's levels (see SHADER_TYPES_DEPTH_PYRAMID_LEVEL_COUNT): read as storage images too, so of a format
// (reading without one needs shaderStorageImageReadWithoutFormat, which KosmicKrisp lacks)
[[vk::binding(1, DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES)]] [[vk::image_format("r32f")]]
RWTexture2D<float> gDepthPyramid[SHADER_TYPES_DEPTH_PYRAMID_LEVEL_COUNT];
[[vk::binding(2, DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES)]] [[vk::image_format("r32f")]]
RWTexture2D<float> gShadowDepthPyramid[SHADER_TYPES_SHADOW_DEPTH_PYRAMID_LEVEL_COUNT];
[[vk::binding(3, DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES)]] [[vk::image_format("rgba16f")]]
RWTexture3D<float4> gGiVolumeImages[SHADER_TYPES_GI_VOLUME_COUNT];

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

// the gpu's culling of the frame's draws (see CullCandidate, ComputeCull)
[[vk::binding(12, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<CullCandidate> gCullCandidates[SHADER_TYPES_FRAME_COUNT];
[[vk::binding(13, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<CullJob> gCullJobs[SHADER_TYPES_FRAME_COUNT];
[[vk::binding(14, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<CullView> gCullViews[SHADER_TYPES_FRAME_COUNT];
[[vk::binding(15, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
RWStructuredBuffer<DrawIndexedIndirectCommand> gCullCommands[SHADER_TYPES_FRAME_COUNT];
[[vk::binding(16, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
RWStructuredBuffer<uint> gCullCounts[SHADER_TYPES_FRAME_COUNT];
// per draw and main view, whether it was seen (1) the last frame that culled it: one buffer, frames run in order
[[vk::binding(17, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
RWStructuredBuffer<uint> gCullVisibility;
// the depth reprojection (see ComputeDepthSave, ComputeDepthReproject): the last frame's depth of what doesn't move (1,
// the far plane, where something moved), a float per pixel, and the frame's depth reprojected from it, a float's bits
// per pixel (an atomic minimum: depths are positive, so their bits order as they do)
[[vk::binding(18, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
RWStructuredBuffer<float> gPreviousDepth;
[[vk::binding(19, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
RWStructuredBuffer<uint> gReprojectedDepth;
// what the occlusion culling decided (CULL_STATISTIC_*), host visible, per frame index
[[vk::binding(20, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
RWStructuredBuffer<uint> gCullStatistics[SHADER_TYPES_FRAME_COUNT];

// the indirect lighting (see GiData): its parameters per frame index, the voxels, their radiance (rgba16f: radiance times
// occupancy, and occupancy) by level, the probes' rays of the frame (rgba16f: the radiance along each, and the distance
// to what it meets in probe spacings) per probe and ray, and the probes' irradiance (an ambient cube: a float4 per face,
// +x, -x, +y, -y, +z, -z, rgb; face 0's w the frames the probe has accumulated, 1's its noise, 2 to 4's its offset, 5's
// the share of its rays that met a surface's back, blended: inside above kGiInsideShare), in two halves (see
// GiData::probeFirst)
[[vk::binding(21, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<GiData> gGi[SHADER_TYPES_FRAME_COUNT];
[[vk::binding(22, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
RWStructuredBuffer<GiVoxel> gGiVoxels;
[[vk::binding(23, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
RWStructuredBuffer<uint2> gGiRadiance;
[[vk::binding(24, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
RWStructuredBuffer<uint2> gGiRays;
[[vk::binding(25, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
RWStructuredBuffer<float4> gGiIrradiance;
// what the voxelizer reads (a uint4 per run of triangles: its first triangle of all, first index, DrawRecord and
// instance count), and the lights per cell of the volume, per frame index
[[vk::binding(26, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<uint4> gGiJobs[SHADER_TYPES_FRAME_COUNT];
[[vk::binding(27, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<uint2> gGiLightCells[SHADER_TYPES_FRAME_COUNT];
[[vk::binding(28, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<uint> gGiLightIndices[SHADER_TYPES_FRAME_COUNT];
// the probes' depth maps (see SHADER_TYPES_GI_DEPTH_SIDE: half2 per texel, the mean distance and the mean of its square),
// in two halves as the irradiance
[[vk::binding(29, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
RWStructuredBuffer<uint> gGiProbeDepth; // per texel: the mean distance, its spread and the nearest (see GiPackDepth)
// the static voxels kept, which the clips that move shift theirs from (see ComputeGiShift)
[[vk::binding(30, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
RWStructuredBuffer<GiVoxel> gGiStaticVoxels;
// per block of voxels of each clip, whether anything is voxelized near it (see SHADER_TYPES_GI_FLAG_LEVEL)
[[vk::binding(31, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
RWStructuredBuffer<uint> gGiFlags;
// what the probes' update counts (GI_STATISTIC_*), host visible, per frame index
[[vk::binding(32, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
RWStructuredBuffer<uint> gGiStatistics[SHADER_TYPES_FRAME_COUNT];
// the BVH of what of the model moves (see gfx::bvh::Moving), per frame index: its nodes and triangles (GiData::bvhMoving)
[[vk::binding(33, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<float4> gBvhMovingNodes[SHADER_TYPES_FRAME_COUNT];
[[vk::binding(34, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<float4> gBvhMovingTriangles[SHADER_TYPES_FRAME_COUNT];
// which of each voxel's 8 probes see it, traced through the BVHs (see ComputeGiResolve), in two halves (this frame's and
// the last's, see GiData::visibilityFirst)
[[vk::binding(35, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
RWStructuredBuffer<uint> gGiVisibility;
// the frame's traced shadows (see ShadowTraceData), per frame index
[[vk::binding(36, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
StructuredBuffer<ShadowTraceData> gShadowTrace[SHADER_TYPES_FRAME_COUNT];
// the pixels the traced shadows' rays are traced for (see ComputeShadowClassify): a count, then the pixels
[[vk::binding(37, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
RWStructuredBuffer<uint> gShadowTraceList;
// per 8x8 tile, whether it is within the filter's reach of a traced pixel (see ComputeShadowClassify)
[[vk::binding(38, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
RWStructuredBuffer<uint> gShadowTraceTiles;
// the surface cache (see GiCacheEntry): per voxel near a surface, the irradiance along the axes its own rays saw,
// accumulated over frames, 3 uint4 an entry, in pages of a block's 64 entries (GiData::cachePageCapacity); the
// filter's records of this frame's active blocks, in two halves (GiData::cacheRecordCapacity); the blocks' pages and
// records (GiData::cacheTableFirst); and the free pages (the count, the records taken this frame, then the pages: see
// GiCachePopPage)
[[vk::binding(39, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
RWStructuredBuffer<uint4> gGiCache;
[[vk::binding(40, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
RWStructuredBuffer<uint4> gGiCacheRecords;
[[vk::binding(41, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
RWStructuredBuffer<uint> gGiCacheBlocks;
[[vk::binding(42, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
RWStructuredBuffer<uint> gGiCacheFree;

