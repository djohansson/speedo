// the resources the main layout binds: push constants, the layer groups' specialization constant, buffers, textures and samplers. a part of shaders.slang, which includes it after capi.h and sky.h
//
// a descriptor set's resources are a parameter block's fields (see docs/backends-plan.md): a descriptor set on Vulkan, an
// argument buffer on Metal, a register space's descriptor table on D3D12. the fields bind in their order (0, 1, 2...), and
// the reflection names each binding by its field (ShaderCompile's CreateLayoutBindings), which gfx binds by
// (Pipeline::SetDescriptorData), so the fields keep the names the globals had, and a define per field lets the other
// parts use them as before

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

struct GlobalBuffersSet
{
	StructuredBuffer<VertexP3fN3fTa4fT014fC4f> gVertexBuffer;
	StructuredBuffer<SkinVertex> gSkinVertices;
	StructuredBuffer<MorphDelta> gMorphDeltas;
	// order independent transparency (see OitNode)
	RWStructuredBuffer<uint> gOitHeads;
	RWStructuredBuffer<OitNode> gOitNodes;
	RWStructuredBuffer<uint> gOitCounter;
	// the frames' histograms, SHADER_TYPES_HISTOGRAM_BINS per frame index: auto exposure's bins, then the distances' (see
	// SHADER_TYPES_DISTANCE_BINS)
	RWStructuredBuffer<uint> gExposureHistogram;
	// the model's indices (the index buffer's), for what reads its triangles in compute (the indirect lighting's voxelizer)
	StructuredBuffer<uint> gIndexBuffer;
	// the model's BVH over what doesn't move (see gfx/bvh.h, bvh.slang): CWBVH nodes and triangles, for the indirect
	// lighting's rays (GiData::trace)
	StructuredBuffer<float4> gBvhNodes;
	StructuredBuffer<float4> gBvhTriangles;
	// the model's emissive triangles that don't move (see EmitterTriangle)
	StructuredBuffer<EmitterTriangle> gEmitters;
	// and the tree they are picked through (see EmitterNode)
	StructuredBuffer<EmitterNode> gEmitterTree;
	// the visibility buffer's tiles sorted by the kernel that shades them (see ComputeMainVisibilityClassify): the
	// specialized kernel's indirect dispatch (its thread group counts, x the tiles), the generic kernel's, then each one's
	// tiles (VisibilityTileFirst), x and y in 16 bits each
	RWStructuredBuffer<uint> gVisibilityTiles;
	// the glare's sources this frame (see ComputeFlareSources, LensSource): how many, then the sources
	RWStructuredBuffer<uint> gLensSourceCount;
	RWStructuredBuffer<LensSource> gLensSources;
};
[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS)]]
ParameterBlock<GlobalBuffersSet> gSetGlobalBuffers;
#define gVertexBuffer gSetGlobalBuffers.gVertexBuffer
#define gSkinVertices gSetGlobalBuffers.gSkinVertices
#define gMorphDeltas gSetGlobalBuffers.gMorphDeltas
#define gOitHeads gSetGlobalBuffers.gOitHeads
#define gOitNodes gSetGlobalBuffers.gOitNodes
#define gOitCounter gSetGlobalBuffers.gOitCounter
#define gExposureHistogram gSetGlobalBuffers.gExposureHistogram
#define gIndexBuffer gSetGlobalBuffers.gIndexBuffer
#define gBvhNodes gSetGlobalBuffers.gBvhNodes
#define gBvhTriangles gSetGlobalBuffers.gBvhTriangles
#define gEmitters gSetGlobalBuffers.gEmitters
#define gEmitterTree gSetGlobalBuffers.gEmitterTree
#define gVisibilityTiles gSetGlobalBuffers.gVisibilityTiles
#define gLensSourceCount gSetGlobalBuffers.gLensSourceCount
#define gLensSources gSetGlobalBuffers.gLensSources

struct GlobalTexturesSet
{
	Texture2D gTextures[SHADER_TYPES_GLOBAL_TEXTURE_COUNT];
	// the main depth's stencil: its first bit marks what moves (see CommandEncoder::SetStencilReference)
	Texture2D<uint> gMainStencil;
	// the indirect lighting's voxels' resolved probes (see ComputeGiResolve), the clips stacked along z: the irradiance
	// along the axes times the weight of the probes seen (per color its faces +x, -x, +y and -y, then red's and green's +z
	// and -z, then blue's and the weight: see GiSample). sampled trilinearly, and
	// written as storage images (gGiVolumeImages), always in the general layout
	Texture3D<float4> gGiVolumes[SHADER_TYPES_GI_VOLUME_COUNT];
	// the visibility buffer (see FragmentPrepass): per pixel the draw record (marked VISIBILITY_EXTENDED), and the triangle
	// (SV_PrimitiveID) and the instance (by DrawRecord::primitiveBits) the pre-pass drew there, which the main pass's shading reconstructs the
	// surface from (ComputeMainVisibility)
	Texture2D<uint2> gVisibility;
	// the aerial perspective's volume (see AerialPerspectiveData): the light the air scatters toward the eye up to each
	// slice (rgb), and what it lets through (rgb), sampled trilinearly, and written as storage images
	// (gAerialPerspectiveImages), always in the general layout
	Texture3D<float4> gAerialPerspective[2];
	// its history (see ComputeAerialPerspective): each slice's light per meter (rgb) and the fog's share of it (a), kept over
	// frames, the frame's and the last's (AerialPerspectiveData::historySlot), the views' stacked along z as the volume's
	Texture3D<float4> gAerialPerspectiveHistory[2];
};
[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES)]]
ParameterBlock<GlobalTexturesSet> gSetGlobalTextures;
#define gTextures gSetGlobalTextures.gTextures
#define gMainStencil gSetGlobalTextures.gMainStencil
#define gGiVolumes gSetGlobalTextures.gGiVolumes
#define gVisibility gSetGlobalTextures.gVisibility
#define gAerialPerspective gSetGlobalTextures.gAerialPerspective
#define gAerialPerspectiveHistory gSetGlobalTextures.gAerialPerspectiveHistory

struct GlobalRwTexturesSet
{
	RWTexture2D gRWTextures[SHADER_TYPES_GLOBAL_RW_TEXTURE_COUNT];
	// the depth pyramid's levels (see SHADER_TYPES_DEPTH_PYRAMID_LEVEL_COUNT): read as storage images too, so of a format
	// (reading without one needs shaderStorageImageReadWithoutFormat, which KosmicKrisp lacks)
	[[vk::image_format("r32f")]] RWTexture2D<float> gDepthPyramid[SHADER_TYPES_DEPTH_PYRAMID_LEVEL_COUNT];
	[[vk::image_format("r32f")]] RWTexture2D<float> gShadowDepthPyramid[SHADER_TYPES_SHADOW_DEPTH_PYRAMID_LEVEL_COUNT];
	[[vk::image_format("rgba16f")]] RWTexture3D<float4> gGiVolumeImages[SHADER_TYPES_GI_VOLUME_COUNT];
	// the main color, which the visibility buffer's shading writes (see ComputeMainVisibility)
	[[vk::image_format("rgba16f")]] RWTexture2D<float4> gMainColor;
	// the shadow transmittance (SHADER_TYPES_SHADOW_TRANSMITTANCE_TEXTURE), which the caustics' photons start from and their
	// light goes in (see ComputeCaustics)
	[[vk::image_format("rgba16f")]] RWTexture2D<float4> gShadowTransmittanceImage;
	[[vk::image_format("rgba16f")]] RWTexture3D<float4> gAerialPerspectiveImages[2];
	[[vk::image_format("rgba16f")]] RWTexture3D<float4> gAerialPerspectiveHistoryImages[2];
	// the bloom's levels (see ComputeBloomDown), a storage view per level of its chain (SHADER_TYPES_BLOOM_TEXTURE sampled)
	[[vk::image_format("rgba16f")]] RWTexture2D<float4> gBloomLevels[SHADER_TYPES_BLOOM_LEVEL_COUNT];
};
[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES)]]
ParameterBlock<GlobalRwTexturesSet> gSetGlobalRwTextures;
#define gRWTextures gSetGlobalRwTextures.gRWTextures
#define gDepthPyramid gSetGlobalRwTextures.gDepthPyramid
#define gShadowDepthPyramid gSetGlobalRwTextures.gShadowDepthPyramid
#define gGiVolumeImages gSetGlobalRwTextures.gGiVolumeImages
#define gMainColor gSetGlobalRwTextures.gMainColor
#define gShadowTransmittanceImage gSetGlobalRwTextures.gShadowTransmittanceImage
#define gAerialPerspectiveImages gSetGlobalRwTextures.gAerialPerspectiveImages
#define gAerialPerspectiveHistoryImages gSetGlobalRwTextures.gAerialPerspectiveHistoryImages
#define gBloomLevels gSetGlobalRwTextures.gBloomLevels

struct GlobalSamplersSet
{
	SamplerState gSamplers[SHADER_TYPES_GLOBAL_SAMPLER_COUNT];
};
[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS)]]
ParameterBlock<GlobalSamplersSet> gSetGlobalSamplers;
#define gSamplers gSetGlobalSamplers.gSamplers

struct ViewSet
{
	StructuredBuffer<ViewData> gViewData[SHADER_TYPES_FRAME_COUNT];
};
[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_VIEW)]]
ParameterBlock<ViewSet> gSetView;
#define gViewData gSetView.gViewData

struct MaterialSet
{
	StructuredBuffer<MaterialData> gMaterialData;
	StructuredBuffer<TextureView> gTextureViews;
};
[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_MATERIAL)]]
ParameterBlock<MaterialSet> gSetMaterial;
#define gMaterialData gSetMaterial.gMaterialData
#define gTextureViews gSetMaterial.gTextureViews

struct ModelInstancesSet
{
	StructuredBuffer<ModelInstance> gModelInstances[SHADER_TYPES_FRAME_COUNT];
	StructuredBuffer<LightData> gLights;
	StructuredBuffer<float4x4> gJointMatrices[SHADER_TYPES_FRAME_COUNT];
	StructuredBuffer<float> gMorphWeights[SHADER_TYPES_FRAME_COUNT];
	StructuredBuffer<EnvironmentData> gEnvironment;
	// the shadows' views, and each light's first (see ShadowData)
	StructuredBuffer<ShadowData> gShadowViews[SHADER_TYPES_FRAME_COUNT];
	StructuredBuffer<uint> gLightShadows[SHADER_TYPES_FRAME_COUNT];
	// clustered lighting (see SHADER_TYPES_CLUSTER_COUNT): per view its clusters' slicing, per cluster its range of light
	// indices, and the indices
	StructuredBuffer<ClusterView> gClusterViews[SHADER_TYPES_FRAME_COUNT];
	StructuredBuffer<uint2> gClusters[SHADER_TYPES_FRAME_COUNT];
	StructuredBuffer<uint> gClusterLights[SHADER_TYPES_FRAME_COUNT];
	// the ui's frosted panels (see UiBlurRect)
	StructuredBuffer<UiBlurRect> gUiBlurRects[SHADER_TYPES_FRAME_COUNT];
	// the frame's draws' per draw values (see DrawRecord)
	StructuredBuffer<DrawRecord> gDrawRecords[SHADER_TYPES_FRAME_COUNT];
	// the gpu's culling of the frame's draws (see CullCandidate, ComputeCull)
	StructuredBuffer<CullCandidate> gCullCandidates[SHADER_TYPES_FRAME_COUNT];
	StructuredBuffer<CullJob> gCullJobs[SHADER_TYPES_FRAME_COUNT];
	StructuredBuffer<CullView> gCullViews[SHADER_TYPES_FRAME_COUNT];
	RWStructuredBuffer<DrawIndexedIndirectCommand> gCullCommands[SHADER_TYPES_FRAME_COUNT];
	RWStructuredBuffer<uint> gCullCounts[SHADER_TYPES_FRAME_COUNT];
	// per draw and main view, whether it was seen (1) the last frame that culled it: one buffer, frames run in order
	RWStructuredBuffer<uint> gCullVisibility;
	// the depth reprojection (see ComputeDepthSave, ComputeDepthReproject): the last frame's depth of what doesn't move (0,
	// the far plane, where something moved), a float per pixel, and the frame's depth reprojected from it, a float's bits
	// per pixel (an atomic maximum, the nearest being the largest, the depth reversed: depths are positive, so their bits
	// order as they do)
	RWStructuredBuffer<float> gPreviousDepth;
	RWStructuredBuffer<uint> gReprojectedDepth;
	// what the occlusion culling decided (CULL_STATISTIC_*), host visible, per frame index
	RWStructuredBuffer<uint> gCullStatistics[SHADER_TYPES_FRAME_COUNT];
	// the indirect lighting (see GiData): its parameters per frame index, the voxels, their radiance (rgba16f: radiance times
	// occupancy, and occupancy) by level, the probes' rays of the frame (rgba16f: the radiance along each, and the distance
	// to what it meets in probe spacings) per probe and ray, and the probes' irradiance (an ambient cube: a float4 per face,
	// +x, -x, +y, -y, +z, -z, rgb; face 0's w the frames the probe has accumulated, 1's its noise, 2 to 4's its offset, 5's
	// the share of its rays that met a surface's back, blended: inside above kGiInsideShare), in two halves (see
	// GiData::probeFirst)
	StructuredBuffer<GiData> gGi[SHADER_TYPES_FRAME_COUNT];
	RWStructuredBuffer<GiVoxel> gGiVoxels;
	// a voxel's radiance, a uint2 each at GiData::voxelFirst: the frame's, the static voxels' direct, and its part the
	// lights don't change (see GiData::voxelFirst)
	RWStructuredBuffer<uint2> gGiRadiance;
	RWStructuredBuffer<uint2> gGiDirect;
	RWStructuredBuffer<uint2> gGiKept;
	RWStructuredBuffer<uint2> gGiRays;
	RWStructuredBuffer<float4> gGiIrradiance;
	// what the voxelizer reads (a uint4 per run of triangles: its first triangle of all, first index, DrawRecord and
	// instance count), and the lights per cell of the volume, per frame index
	StructuredBuffer<uint4> gGiJobs[SHADER_TYPES_FRAME_COUNT];
	StructuredBuffer<uint2> gGiLightCells[SHADER_TYPES_FRAME_COUNT];
	StructuredBuffer<uint> gGiLightIndices[SHADER_TYPES_FRAME_COUNT];
	// the probes' depth maps (see SHADER_TYPES_GI_DEPTH_SIDE: half2 per texel, the mean distance and the mean of its square),
	// in two halves as the irradiance
	RWStructuredBuffer<uint> gGiProbeDepth; // per texel: the mean distance, its spread and the nearest (see GiPackDepth)
	// the static voxels kept, which the clips that move shift theirs from (see ComputeGiShift)
	RWStructuredBuffer<GiVoxel> gGiStaticVoxels;
	// per block of voxels of each clip, whether anything is voxelized near it (see SHADER_TYPES_GI_FLAG_LEVEL)
	RWStructuredBuffer<uint> gGiFlags;
	// what the probes' update counts (GI_STATISTIC_*), host visible, per frame index
	RWStructuredBuffer<uint> gGiStatistics[SHADER_TYPES_FRAME_COUNT];
	// the BVH of what of the model moves (see gfx::bvh::Moving), per frame index: its nodes and triangles (GiData::bvhMoving)
	StructuredBuffer<float4> gBvhMovingNodes[SHADER_TYPES_FRAME_COUNT];
	StructuredBuffer<float4> gBvhMovingTriangles[SHADER_TYPES_FRAME_COUNT];
	// which of each voxel's 8 probes see it, traced through the BVHs (see ComputeGiResolve), in two halves (this frame's and
	// the last's, see GiData::visibilityFirst)
	RWStructuredBuffer<uint> gGiVisibility;
	// the frame's traced shadows (see ShadowTraceData), per frame index
	StructuredBuffer<ShadowTraceData> gShadowTrace[SHADER_TYPES_FRAME_COUNT];
	// the pixels the traced shadows' rays are traced for (see ComputeShadowClassify): a count, then the pixels
	RWStructuredBuffer<uint> gShadowTraceList;
	// per 8x8 tile, whether it is within the filter's reach of a traced pixel (see ComputeShadowClassify)
	RWStructuredBuffer<uint> gShadowTraceTiles;
	// the surface cache (see GiCacheEntry): per voxel near a surface, the irradiance along the axes its own rays saw,
	// accumulated over frames, 3 uint4 an entry, in pages of a block's 64 entries (GiData::cachePageCapacity); the
	// filter's records of this frame's active blocks, in two halves (GiData::cacheRecordCapacity); the blocks' pages and
	// records (GiData::cacheTableFirst); and the free pages (the count, the records taken this frame, then the pages: see
	// GiCachePopPage)
	RWStructuredBuffer<uint4> gGiCache;
	RWStructuredBuffer<uint4> gGiCacheRecords;
	RWStructuredBuffer<uint> gGiCacheBlocks;
	RWStructuredBuffer<uint> gGiCacheFree;
	// the frame's emitters (see EmitterData), per frame index
	StructuredBuffer<EmitterData> gEmitterData[SHADER_TYPES_FRAME_COUNT];
	// the emitters' reservoirs per traced pixel (see EmitterReservoir): the frame's before their spatial reuse, then the
	// frame's and the last frame's after it, by the frame's parity (EmitterData::frame)
	RWStructuredBuffer<uint4> gEmitterReservoirs;
	// temporal upscaling's reconstructed previous depth (see ComputeUpscaleReconstruct): per render pixel the nearest depth
	// of the points that were there the last frame, a float's bits (an atomic maximum: the depth reversed), cleared to the
	// far plane each frame
	RWStructuredBuffer<uint> gUpscaleDepth;
	// how the visibility buffer's tiles were sorted (VISIBILITY_STATISTIC_*), host visible, per frame index: what decides whether
	// the next frames sort them (see Renderer::myVisibilityPlainShare)
	RWStructuredBuffer<uint> gVisibilityStatistics[SHADER_TYPES_FRAME_COUNT];
	// the caustics' photons' light as they land, per texel of the shadow transmittance and channel, in fixed point (see
	// SHADER_TYPES_SHADOWS_CAUSTICS), emptied each frame they are traced in
	RWStructuredBuffer<uint> gCausticPhotons;
	// the frame's aerial perspective (see AerialPerspectiveData), per frame index
	StructuredBuffer<AerialPerspectiveData> gAerialPerspectiveData[SHADER_TYPES_FRAME_COUNT];
	// the camera's lens (see LensData): what the Lens pass and the flares take, per frame index. gLensData[frame][0]
	StructuredBuffer<LensData> gLensData[SHADER_TYPES_FRAME_COUNT];
	// and its ghosts (see LensGhost), LensData::ghostCount of them, per frame index
	StructuredBuffer<LensGhost> gLensGhosts[SHADER_TYPES_FRAME_COUNT];
#if BVH_RAY_QUERY
	// where the device's ray queries trace instead (see gfx/accelerationstructures.h), its top level structure per frame index:
	// what doesn't move (gBvhTriangles' triangles) and what moves (gBvhMovingTriangles[frame]'s), see BvhTraceScene. last, as
	// the watch's, so that the other bindings are the same with ray queries and without
	RaytracingAccelerationStructure gBvhScene[SHADER_TYPES_FRAME_COUNT];
#if BVH_RAY_QUERY_WATCH
	// diagnostic: what the ray queries do, host visible (see RAY_QUERY_WATCH_BEGUN)
	RWStructuredBuffer<uint> gRayQueryWatch;
	// diagnostic: the rays replayed (see ComputeRayQueryReplay): their count in the first's x, then two a ray
	StructuredBuffer<float4> gRayQueryReplay;
#endif
#endif
};
[[vk::binding(0, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES)]]
ParameterBlock<ModelInstancesSet> gSetModelInstances;
#define gModelInstances gSetModelInstances.gModelInstances
#define gLights gSetModelInstances.gLights
#define gJointMatrices gSetModelInstances.gJointMatrices
#define gMorphWeights gSetModelInstances.gMorphWeights
#define gEnvironment gSetModelInstances.gEnvironment
#define gShadowViews gSetModelInstances.gShadowViews
#define gLightShadows gSetModelInstances.gLightShadows
#define gClusterViews gSetModelInstances.gClusterViews
#define gClusters gSetModelInstances.gClusters
#define gClusterLights gSetModelInstances.gClusterLights
#define gUiBlurRects gSetModelInstances.gUiBlurRects
#define gDrawRecords gSetModelInstances.gDrawRecords
#define gCullCandidates gSetModelInstances.gCullCandidates
#define gCullJobs gSetModelInstances.gCullJobs
#define gCullViews gSetModelInstances.gCullViews
#define gCullCommands gSetModelInstances.gCullCommands
#define gCullCounts gSetModelInstances.gCullCounts
#define gCullVisibility gSetModelInstances.gCullVisibility
#define gPreviousDepth gSetModelInstances.gPreviousDepth
#define gReprojectedDepth gSetModelInstances.gReprojectedDepth
#define gCullStatistics gSetModelInstances.gCullStatistics
#define gGi gSetModelInstances.gGi
#define gGiVoxels gSetModelInstances.gGiVoxels
#define gGiRadiance gSetModelInstances.gGiRadiance
#define gGiDirect gSetModelInstances.gGiDirect
#define gGiKept gSetModelInstances.gGiKept
#define gGiRays gSetModelInstances.gGiRays
#define gGiIrradiance gSetModelInstances.gGiIrradiance
#define gGiJobs gSetModelInstances.gGiJobs
#define gGiLightCells gSetModelInstances.gGiLightCells
#define gGiLightIndices gSetModelInstances.gGiLightIndices
#define gGiProbeDepth gSetModelInstances.gGiProbeDepth
#define gGiStaticVoxels gSetModelInstances.gGiStaticVoxels
#define gGiFlags gSetModelInstances.gGiFlags
#define gGiStatistics gSetModelInstances.gGiStatistics
#define gBvhMovingNodes gSetModelInstances.gBvhMovingNodes
#define gBvhMovingTriangles gSetModelInstances.gBvhMovingTriangles
#define gGiVisibility gSetModelInstances.gGiVisibility
#define gShadowTrace gSetModelInstances.gShadowTrace
#define gShadowTraceList gSetModelInstances.gShadowTraceList
#define gShadowTraceTiles gSetModelInstances.gShadowTraceTiles
#define gGiCache gSetModelInstances.gGiCache
#define gGiCacheRecords gSetModelInstances.gGiCacheRecords
#define gGiCacheBlocks gSetModelInstances.gGiCacheBlocks
#define gGiCacheFree gSetModelInstances.gGiCacheFree
#define gEmitterData gSetModelInstances.gEmitterData
#define gEmitterReservoirs gSetModelInstances.gEmitterReservoirs
#define gUpscaleDepth gSetModelInstances.gUpscaleDepth
#define gVisibilityStatistics gSetModelInstances.gVisibilityStatistics
#define gCausticPhotons gSetModelInstances.gCausticPhotons
#define gAerialPerspectiveData gSetModelInstances.gAerialPerspectiveData
#define gLensData gSetModelInstances.gLensData
#define gLensGhosts gSetModelInstances.gLensGhosts
#if BVH_RAY_QUERY
#define gBvhScene gSetModelInstances.gBvhScene
#if BVH_RAY_QUERY_WATCH
#define gRayQueryWatch gSetModelInstances.gRayQueryWatch
#define gRayQueryReplay gSetModelInstances.gRayQueryReplay
#endif
#endif
