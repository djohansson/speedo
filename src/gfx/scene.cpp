#include "scene.h"

#include <core/application.h>
#include <core/assert.h>
#include <core/profiling.h>
#include <core/task.h>
#include <core/taskexecutor.h>
#include <gfx/accelerationstructures.h>
#include <gfx/environment.h>
#include <gfx/environmentfilter.h>
#include <gfx/lens.h>
#include <gfx/gpu.h>
#include <gfx/graphicsqueue.h>
#include <gfx/meshimport.h>
#include <gfx/model.h>
#include <gfx/rayquerywatch.h>
#include <gfx/renderer.h>
#include <gfx/shaderloader.h>
#include <gfx/shaders/capi.h>
#include <gfx/texture.h>
#include <gfx/views.h>
#include <platform/capi.h>
#include <rhi/capi.h>

#include <uuid.h>
#include <xxhash.h>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <bit>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <utility>

namespace gfx
{

using namespace rhi;

// the layers a material draws: those on at rest, and those an animation turns on (KHR_animation_pointer: a factor that
// animates up from 0)
struct MaterialLayers
{
	bool clearcoat = false;
	bool sheen = false;
	bool transmission = false;
	bool anisotropy = false;
	bool iridescence = false;
	bool diffuseTransmission = false;
	bool retroreflection = false;
	bool volumeScatter = false;
};


[[nodiscard]] static MaterialLayers LayersOf(const ModelDesc& desc, size_t materialIndex)
{
	const auto& material = desc.materials[materialIndex];
	MaterialLayers layers{
		.clearcoat = material.clearcoat > 0.0F,
		.sheen = glm::any(glm::greaterThan(material.sheenColor, glm::vec3(0.0F))),
		.transmission = material.transmission > 0.0F,
		.anisotropy = material.anisotropy > 0.0F,
		.iridescence = material.iridescence > 0.0F,
		.diffuseTransmission = material.diffuseTransmission > 0.0F,
		.retroreflection = material.retroreflection > 0.0F,
		.volumeScatter = material.volumeScatter};
	for (const auto& target : desc.animation.pointerTargets)
	{
		if (target.kind != ScenePointerTarget::Kind::kMaterial || target.index != materialIndex)
			continue;
		switch (MaterialProperty{target.property})
		{
		case MaterialProperty::kClearcoat: layers.clearcoat = true; break;
		case MaterialProperty::kSheenColor: layers.sheen = true; break;
		case MaterialProperty::kTransmission: layers.transmission = true; break;
		case MaterialProperty::kAnisotropy: layers.anisotropy = true; break;
		case MaterialProperty::kIridescence: layers.iridescence = true; break;
		case MaterialProperty::kDiffuseTransmission: layers.diffuseTransmission = true; break;
		default: break;
		}
	}
	return layers;
}



// what the descriptor sets of the pipeline (see shaders/capi.h) are allocated from: room for many copies of the
// global arrays, since every change to one takes a new descriptor set
static std::vector<DescriptorPoolSize> DescriptorPoolSizes()
{
	constexpr uint32_t kGlobalResourceBaseCount = 128;
	constexpr uint32_t kBufferBaseCount = kGlobalResourceBaseCount * 1024;

	return {
		{.type = rhi::DescriptorType::kSampler, .count = kGlobalResourceBaseCount * SHADER_TYPES_GLOBAL_SAMPLER_COUNT},
		{.type = rhi::DescriptorType::kCombinedImageSampler, .count = kGlobalResourceBaseCount * SHADER_TYPES_GLOBAL_SAMPLER_COUNT},
		{.type = rhi::DescriptorType::kSampledImage,
		 // gTextures, gMainStencil, gGiVolumes, gVisibility, gAerialPerspective and gAerialPerspectiveHistory
		 .count = kGlobalResourceBaseCount * (SHADER_TYPES_GLOBAL_TEXTURE_COUNT + 1 + SHADER_TYPES_GI_VOLUME_COUNT + 1 + 2 + 2)},
		{.type = rhi::DescriptorType::kStorageImage,
		 // gRWTextures, gDepthPyramid, gShadowDepthPyramid, gGiVolumeImages, gMainColor, gShadowTransmittanceImage,
		 // gAerialPerspectiveImages and gAerialPerspectiveHistoryImages
		 .count = kGlobalResourceBaseCount * (SHADER_TYPES_GLOBAL_RW_TEXTURE_COUNT + SHADER_TYPES_DEPTH_PYRAMID_LEVEL_COUNT +
											  SHADER_TYPES_SHADOW_DEPTH_PYRAMID_LEVEL_COUNT + SHADER_TYPES_GI_VOLUME_COUNT + 2 + 2 + 2)},
		{.type = rhi::DescriptorType::kUniformTexelBuffer, .count = kBufferBaseCount},
		{.type = rhi::DescriptorType::kStorageTexelBuffer, .count = kBufferBaseCount},
		{.type = rhi::DescriptorType::kUniformBuffer, .count = kBufferBaseCount},
		{.type = rhi::DescriptorType::kStorageBuffer, .count = kBufferBaseCount},
		{.type = rhi::DescriptorType::kUniformBufferDynamic, .count = kBufferBaseCount},
		{.type = rhi::DescriptorType::kStorageBufferDynamic, .count = kBufferBaseCount},
		{.type = rhi::DescriptorType::kInputAttachment, .count = kBufferBaseCount},
		// gBvhScene, a frame index's each, in a set of the model's buffers, which takes a new one whenever one of them
		// changes: as many as of the buffers (the pipeline leaves the type out where the device has no ray queries)
		{.type = rhi::DescriptorType::kAccelerationStructure, .count = kBufferBaseCount},
	};
}


// gTextures slot 0 is the renderer's main color (SHADER_TYPES_RENDER_TARGET_TEXTURE), 1 to 3 temporal upscaling's
// (SHADER_TYPES_COMPOSITE_TEXTURE, SHADER_TYPES_UPSCALE_COLOR_TEXTURE), and
// SHADER_TYPES_ENVIRONMENT_TEXTURE and SHADER_TYPES_ENVIRONMENT_SHEEN_TEXTURE the environment's (and
// SHADER_TYPES_TRANSMISSION_TEXTURE the opaque scene). material 0 is the default material, for models (or parts of them) without one: it samples this slot, which opening an image replaces.
static constexpr uint32_t kMaterialTextureId = 15;

// the loaded model's materials are 1 and up, and their textures are in the slots from here up
static constexpr uint32_t kModelTextureFirstSlot = 85; // after the sky's tables, its views, its view by the moon, the lens pass's image, the glare's, the depth of field's and the motion blur's (SHADER_TYPES_MOTION_TILES_DILATED_TEXTURE)

static constexpr uint32_t kModelTextureMaxCount = SHADER_TYPES_GLOBAL_TEXTURE_COUNT - kModelTextureFirstSlot;

static constexpr uint32_t kDefaultSamplerId = 2;

// the sampler slots a model's samplers go in: all but the default's and the clamping one's
// (SHADER_TYPES_CLAMP_SAMPLER)
static_assert(kDefaultSamplerId != SHADER_TYPES_CLAMP_SAMPLER);

static constexpr auto kModelSamplerSlots = []
{
	std::array<uint32_t, SHADER_TYPES_GLOBAL_SAMPLER_COUNT - 2> slots{};
	for (uint32_t slot = 0, slotIt = 0; slot < SHADER_TYPES_GLOBAL_SAMPLER_COUNT; slot++)
		if (slot != kDefaultSamplerId && slot != SHADER_TYPES_CLAMP_SAMPLER)
			slots[slotIt++] = slot;
	return slots;
}();

static_assert(
	kMaterialTextureId >= SHADER_TYPES_FRAME_COUNT && kMaterialTextureId != SHADER_TYPES_ENVIRONMENT_TEXTURE &&
	kMaterialTextureId != SHADER_TYPES_ENVIRONMENT_SHEEN_TEXTURE && kMaterialTextureId != SHADER_TYPES_TRANSMISSION_TEXTURE &&
	SHADER_TYPES_ENVIRONMENT_TEXTURE >= SHADER_TYPES_FRAME_COUNT && SHADER_TYPES_ENVIRONMENT_SHEEN_TEXTURE >= SHADER_TYPES_FRAME_COUNT &&
	kMaterialTextureId < kModelTextureFirstSlot);
static_assert(SHADER_TYPES_UI_BLUR_SOURCE_TEXTURE < kModelTextureFirstSlot && SHADER_TYPES_UI_BLUR_ROWS_TEXTURE < kModelTextureFirstSlot);
static_assert(SHADER_TYPES_PREPASS_NORMAL_TEXTURE < kModelTextureFirstSlot && SHADER_TYPES_SHADOW_MASK_TEXTURE < kModelTextureFirstSlot);
static_assert(
	SHADER_TYPES_GI_NEAR_RAW_TEXTURE < kModelTextureFirstSlot && SHADER_TYPES_GI_NEAR_ROWS_TEXTURE < kModelTextureFirstSlot &&
	SHADER_TYPES_GI_NEAR_TEXTURE < kModelTextureFirstSlot);
static_assert(
	SHADER_TYPES_GI_NEAR_HISTORY_DISTANCE_TEXTURE + 1 < kModelTextureFirstSlot && SHADER_TYPES_EMITTER_RAW_TEXTURE < kModelTextureFirstSlot &&
	SHADER_TYPES_EMITTER_SURFACE_TEXTURE + 1 < kModelTextureFirstSlot);
static_assert(SHADER_TYPES_EMITTER_SURFACE_RW_TEXTURE + 1 < SHADER_TYPES_GLOBAL_RW_TEXTURE_COUNT);
static_assert(
	SHADER_TYPES_REFLECTION_RAW_TEXTURE > SHADER_TYPES_EMITTER_PIXEL_SURFACE_TEXTURE + 1 &&
	SHADER_TYPES_REFLECTION_SURFACE_TEXTURE + 1 < kModelTextureFirstSlot &&
	SHADER_TYPES_REFLECTION_RAW_RW_TEXTURE > SHADER_TYPES_EMITTER_PIXEL_SURFACE_RW_TEXTURE + 1 &&
	SHADER_TYPES_REFLECTION_SURFACE_RW_TEXTURE + 1 < SHADER_TYPES_GLOBAL_RW_TEXTURE_COUNT);
static_assert(
	SHADER_TYPES_COMPOSITE_TEXTURE > SHADER_TYPES_RENDER_TARGET_TEXTURE && SHADER_TYPES_UPSCALE_COLOR_TEXTURE + 1 < SHADER_TYPES_MAIN_DEPTH_TEXTURE &&
	SHADER_TYPES_UPSCALE_COLOR_RW_TEXTURE > SHADER_TYPES_EMITTER_SURFACE_RW_TEXTURE + 1 && SHADER_TYPES_COMPOSITE_RW_TEXTURE < SHADER_TYPES_GLOBAL_RW_TEXTURE_COUNT);
static_assert(SHADER_TYPES_SKY_VIEW_TEXTURE > SHADER_TYPES_EMITTER_SURFACE_TEXTURE + 1 && SHADER_TYPES_SKY_VIEW_TEXTURE < kModelTextureFirstSlot);
static_assert(
	SHADER_TYPES_SHADOW_TRANSMITTANCE_TEXTURE > SHADER_TYPES_REFLECTION_SURFACE_TEXTURE + 1 &&
	SHADER_TYPES_SHADOW_TRANSMITTANCE_TEXTURE < kModelTextureFirstSlot);
static_assert(
	SHADER_TYPES_REFLECTION_RAY_TEXTURE > SHADER_TYPES_SHADOW_TRANSMITTANCE_TEXTURE && SHADER_TYPES_REFLECTION_COAT_TEXTURE < kModelTextureFirstSlot &&
	SHADER_TYPES_REFLECTION_RAY_RW_TEXTURE > SHADER_TYPES_TRANSMISSION_RW_TEXTURE &&
	SHADER_TYPES_REFLECTION_COAT_RW_TEXTURE < SHADER_TYPES_GLOBAL_RW_TEXTURE_COUNT);
static_assert(SHADER_TYPES_PREPASS_MOTION_TEXTURE > SHADER_TYPES_SKY_VIEW_TEXTURE && SHADER_TYPES_PREPASS_MOTION_TEXTURE < kModelTextureFirstSlot);
static_assert(
	SHADER_TYPES_SKY_TRANSMITTANCE_TEXTURE > SHADER_TYPES_REFLECTION_COAT_TEXTURE &&
	SHADER_TYPES_SKY_MULTIPLE_SCATTERING_TEXTURE == SHADER_TYPES_SKY_TRANSMITTANCE_TEXTURE + 1 &&
	SHADER_TYPES_SKY_MULTIPLE_SCATTERING_TEXTURE < kModelTextureFirstSlot);
static_assert(
	SHADER_TYPES_SKY_EYE_VIEW_TEXTURE == SHADER_TYPES_SKY_MULTIPLE_SCATTERING_TEXTURE + 1 && SHADER_TYPES_SKY_EYE_VIEW_TEXTURE < kModelTextureFirstSlot &&
	SHADER_TYPES_SKY_EYE_VIEW_RW_TEXTURE > SHADER_TYPES_REFLECTION_COAT_RW_TEXTURE && SHADER_TYPES_SKY_EYE_VIEW_RW_TEXTURE < SHADER_TYPES_GLOBAL_RW_TEXTURE_COUNT);
static_assert(SHADER_TYPES_SKY_MOON_VIEW_TEXTURE == SHADER_TYPES_SKY_EYE_VIEW_TEXTURE + 1 && SHADER_TYPES_SKY_MOON_VIEW_TEXTURE < kModelTextureFirstSlot);
static_assert(
	SHADER_TYPES_LENSED_TEXTURE == SHADER_TYPES_SKY_MOON_VIEW_TEXTURE + 1 && SHADER_TYPES_LENSED_TEXTURE < kModelTextureFirstSlot &&
	SHADER_TYPES_LENSED_RW_TEXTURE > SHADER_TYPES_SKY_EYE_VIEW_RW_TEXTURE && SHADER_TYPES_LENSED_RW_TEXTURE < SHADER_TYPES_GLOBAL_RW_TEXTURE_COUNT);
static_assert(
	SHADER_TYPES_BLOOM_TEXTURE == SHADER_TYPES_LENSED_TEXTURE + 1 && SHADER_TYPES_FLARE_TEXTURE == SHADER_TYPES_BLOOM_TEXTURE + 1 &&
	SHADER_TYPES_FLARE_TEXTURE < kModelTextureFirstSlot);
static_assert(
	SHADER_TYPES_DOF_TEXTURE == SHADER_TYPES_FLARE_TEXTURE + 1 && SHADER_TYPES_DOF_TILES_DILATED_TEXTURE == SHADER_TYPES_DOF_TEXTURE + 4 &&
	SHADER_TYPES_DOF_TILES_DILATED_TEXTURE < kModelTextureFirstSlot && SHADER_TYPES_DOF_RW_TEXTURE == SHADER_TYPES_LENSED_RW_TEXTURE + 1 &&
	SHADER_TYPES_DOF_TILES_DILATED_RW_TEXTURE == SHADER_TYPES_DOF_RW_TEXTURE + 4 && SHADER_TYPES_DOF_TILES_DILATED_RW_TEXTURE < SHADER_TYPES_GLOBAL_RW_TEXTURE_COUNT);
static_assert(
	SHADER_TYPES_MOTION_BLUR_TEXTURE == SHADER_TYPES_DOF_TILES_DILATED_TEXTURE + 1 && SHADER_TYPES_MOTION_TILES_DILATED_TEXTURE == SHADER_TYPES_MOTION_BLUR_TEXTURE + 3 &&
	SHADER_TYPES_MOTION_TILES_DILATED_TEXTURE < kModelTextureFirstSlot && SHADER_TYPES_MOTION_BLUR_RW_TEXTURE == SHADER_TYPES_DOF_TILES_DILATED_RW_TEXTURE + 1 &&
	SHADER_TYPES_MOTION_TILES_DILATED_RW_TEXTURE == SHADER_TYPES_MOTION_BLUR_RW_TEXTURE + 3 && SHADER_TYPES_MOTION_TILES_DILATED_RW_TEXTURE < SHADER_TYPES_GLOBAL_RW_TEXTURE_COUNT);
static_assert(
	SHADER_TYPES_UPSCALE_DILATED_DEPTH_TEXTURE > SHADER_TYPES_PREPASS_MOTION_TEXTURE && SHADER_TYPES_UPSCALE_NEW_LOCKS_TEXTURE < kModelTextureFirstSlot &&
	SHADER_TYPES_UPSCALE_DILATED_DEPTH_RW_TEXTURE > SHADER_TYPES_COMPOSITE_RW_TEXTURE &&
	SHADER_TYPES_UPSCALE_NEW_LOCKS_RW_TEXTURE < SHADER_TYPES_GLOBAL_RW_TEXTURE_COUNT);
static_assert(
	SHADER_TYPES_EMITTER_PIXEL_SURFACE_TEXTURE > SHADER_TYPES_UPSCALE_NEW_LOCKS_TEXTURE && SHADER_TYPES_EMITTER_PIXEL_SURFACE_TEXTURE + 1 < kModelTextureFirstSlot &&
	SHADER_TYPES_EMITTER_PIXEL_SURFACE_RW_TEXTURE > SHADER_TYPES_UPSCALE_NEW_LOCKS_RW_TEXTURE && SHADER_TYPES_EMITTER_PIXEL_SURFACE_RW_TEXTURE + 1 < SHADER_TYPES_GLOBAL_RW_TEXTURE_COUNT);


// what makes texture views the same, to share them: the transform by its bits, so that the key hashes as bytes
struct TextureViewKey
{
	uint32_t textureSlot = 0;
	uint32_t samplerSlot = 0;
	uint32_t texCoord = 0;
	std::array<uint32_t, 6> transform{};
	// 1 + the TextureRef::animatedTransform of a texture whose transform animates (its view is its own), else 0
	uint32_t animated = 0;
	uint32_t flags = 0; // TextureView::flags

	[[nodiscard]] bool operator==(const TextureViewKey&) const = default;
};

static_assert(std::has_unique_object_representations_v<TextureViewKey>, "TextureViewKey is hashed as bytes");


struct TextureViewKeyHash
{
	using is_avalanching = void; //NOLINT(readability-identifier-naming)

	[[nodiscard]] uint64_t operator()(const TextureViewKey& key) const noexcept { return XXH3_64bits(&key, sizeof(key)); }
};


// a view of a texture slot with a sampler slot, as a TextureRef samples it
[[nodiscard]] static TextureView MakeTextureView(uint32_t textureSlot, uint32_t samplerSlot, const TextureRef& ref, uint32_t flags = 0)
{
	const auto& tRef = ref.transform;
	return TextureView{
		.uTransform = {tRef[0], tRef[1], tRef[2], 0.0F},
		.vTransform = {tRef[3], tRef[4], tRef[5], 0.0F},
		.textureId = textureSlot,
		.samplerId = samplerSlot,
		.texCoordSet = ref.texCoord,
		.flags = flags,};
}


// a material's textures, null where it has none (or it failed to load)
struct MaterialTextures
{
	Texture diffuse;
	Texture alpha;
	Texture normal;
	Texture emissive;
	Texture occlusion;
	Texture metallicRoughness;
	Texture specular;
	Texture specularColor;
	Texture clearcoat;
	Texture clearcoatRoughness;
	Texture clearcoatNormal;
	Texture sheenColor;
	Texture sheenRoughness;
	Texture transmission;
	Texture thickness;
	Texture anisotropy;
	Texture iridescence;
	Texture iridescenceThickness;
	Texture diffuseTransmission;
	Texture diffuseTransmissionColor;
	Texture retroreflection;
	Texture multiscatterColor;
};


// the installed model's materials as uploaded (gMaterialData from 1), and its texture views whose transforms animate
// (KHR_animation_pointer), which ApplyPointerValues patches. draw thread only.
struct AnimatedTextureView
{
	uint32_t view = 0; // in gTextureViews
	uint32_t target = 0; // its offset's ScenePointerTarget (see TextureRef::animatedTransform)
	TextureView data; // as uploaded
};


// a material value a KHR_animation_pointer channel sets (see ScenePointerTarget::Kind::kMaterial), into its MaterialData
static void ApplyMaterialProperty(MaterialData& data, const ModelMaterial& desc, MaterialProperty property, std::span<const float> v, float scale)
{
	auto copy = [&v](float* out, size_t count) { std::copy_n(v.begin(), std::min(count, v.size()), out); };
	if (v.empty())
		return;
	switch (property)
	{
	case MaterialProperty::kBaseColor: copy(data.color, 4); break;
	case MaterialProperty::kEmissive:
		for (size_t channel = 0; channel < std::min<size_t>(3, v.size()); channel++)
			data.emissive[channel] = v[channel] * scale;
		break;
	case MaterialProperty::kMetallic:
		if (!desc.specularGlossiness)
			data.metallic = v[0];
		break;
	case MaterialProperty::kRoughness:
		if (!desc.specularGlossiness)
			data.roughness = v[0];
		break;
	case MaterialProperty::kAlphaCutoff:
		if (desc.alphaCutoff > 0.0F) // only masked materials test it
			data.alphaCutoff = v[0];
		break;
	case MaterialProperty::kNormalScale: data.normalScale = v[0]; break;
	case MaterialProperty::kOcclusionStrength: data.emissive[3] = v[0]; break;
	case MaterialProperty::kSpecular: data.specular = v[0]; break;
	case MaterialProperty::kSpecularColor: copy(data.specularColor, 3); break;
	case MaterialProperty::kIor: data.specularColor[3] = v[0]; break;
	case MaterialProperty::kClearcoat: data.clearcoat[0] = v[0]; break;
	case MaterialProperty::kClearcoatRoughness: data.clearcoat[1] = v[0]; break;
	case MaterialProperty::kClearcoatNormalScale: data.clearcoat[2] = v[0]; break;
	case MaterialProperty::kSheenColor: copy(data.sheen, 3); break;
	case MaterialProperty::kSheenRoughness: data.sheen[3] = v[0]; break;
	case MaterialProperty::kTransmission: data.transmission[0] = v[0]; break;
	case MaterialProperty::kThickness: data.transmission[1] = v[0]; break;
	case MaterialProperty::kAttenuationDistance: data.transmission[2] = v[0]; break;
	case MaterialProperty::kDispersion: data.transmission[3] = v[0]; break;
	case MaterialProperty::kAttenuationColor: copy(data.attenuationColor, 3); break;
	case MaterialProperty::kAnisotropy: data.anisotropy[0] = v[0]; break;
	case MaterialProperty::kIridescence: data.iridescence[0] = v[0]; break;
	case MaterialProperty::kIridescenceIor: data.iridescence[1] = v[0]; break;
	case MaterialProperty::kIridescenceThicknessMin: data.iridescence[2] = v[0]; break;
	case MaterialProperty::kIridescenceThicknessMax: data.iridescence[3] = v[0]; break;
	case MaterialProperty::kDiffuseTransmission: data.diffuseTransmission[3] = v[0]; break;
	case MaterialProperty::kDiffuseTransmissionColor: copy(data.diffuseTransmission, 3); break;
	case MaterialProperty::kAnisotropyRotation:
		data.anisotropy[1] = std::cos(v[0]);
		data.anisotropy[2] = std::sin(v[0]);
		break;
	case MaterialProperty::kEmissiveStrength: break; // with kEmissive
	}
}


// a material's defaults: white, untextured, rough, dielectric, with the default specular (ior 1.5, white)
[[nodiscard]] static MaterialData DefaultMaterialData()
{
	MaterialData material{};
	std::ranges::fill(material.color, 1.0F);
	material.specularColor[0] = material.specularColor[1] = material.specularColor[2] = 1.0F;
	material.specularColor[3] = 1.5F;
	material.roughness = 1.0F;
	material.specular = 1.0F;
	return material;
}

struct Scene::Impl
{
	RHI& myRHI;
	// the pipeline the scene draws with: its descriptor sets hold the scene's bindings, so another scene needs one of its own
	Pipeline& myPipeline;
	Views& myViews;
	Settings mySettings;

	// draws the frames (see Renderer): its frame graph owns what is sized by the swapchain
	std::unique_ptr<Renderer> myRenderer;
	// where the scene's frames go: the window's swapchain, or an image (see RenderOutput)
	RenderOutput& myOutput;
	// prefilters the environments on the gpu (see InstallEnvironment)
	std::unique_ptr<EnvironmentFilter> myEnvironmentFilter;
	// the bytes of device memory its transients take, and would take without aliasing. written on resize, for the ui
	std::atomic_uint64_t myTransientMemory;
	std::atomic_uint64_t myUnaliasedTransientMemory;
	std::shared_ptr<Model> myModel; // the loaded model, see InstallModel. only the draw thread uses it
	uuids::uuid myLoadedImageUuid; // the loaded image and its view, see InstallImage. nil until one is loaded
	uuids::uuid myLoadedImageViewUuid;
	uuids::uuid myBlackTextureUuid;
	uuids::uuid myBlackTextureViewUuid;
	// the environment (see InstallEnvironment): gEnvironment's buffer, and the prefiltered panorama in
	// SHADER_TYPES_ENVIRONMENT_TEXTURE (and for sheen, SHADER_TYPES_ENVIRONMENT_SHEEN_TEXTURE), nil until one is
	// installed. its name, for the menu
	uuids::uuid myEnvironmentUuid;
	uuids::uuid myEnvironmentImageUuid;
	uuids::uuid myEnvironmentViewUuid;
	uuids::uuid myEnvironmentSheenImageUuid;
	uuids::uuid myEnvironmentSheenViewUuid;
	core::ConcurrentAccess<std::string> myEnvironmentName;
	core::ConcurrentAccess<std::string> myEnvironmentPath; // its file, empty for the sky
	// the installed environment if it is the sky, which UpdateSky draws again as the time of day moves its sun (see
	// kSkyRedrawAngle) and as its air changes, the sun and the aerosols it was last drawn for, and the time of day's clock
	// (draw thread)
	std::shared_ptr<const EnvironmentTexture> mySky;
	glm::vec3 mySkySunDrawn{0.0F};
	glm::vec3 mySkyMoonDrawn{0.0F};
	float mySkyAerosolsDrawn = 1.0F;
	// the moon's illuminance at the top of the atmosphere at its phase as the sky was last drawn, and whether the sun lit
	// the ground then (above the horizon): the air's light is the sun's else the moon's (FrameInputs::airLight)
	float mySkyMoonIlluminanceDrawn = 0.0F;
	bool mySkySunUpDrawn = true;
	// the sun and the moon gEnvironment's sky fields were last uploaded for (see kSkyUpdateAngle)
	glm::vec3 mySkySunUploaded{0.0F};
	glm::vec3 mySkyMoonUploaded{0.0F};
	std::chrono::steady_clock::time_point myTimeLast = std::chrono::steady_clock::now();
	std::atomic<bool> mySkyInstalled = false;

	uuids::uuid mySamplersUuid;
	uuids::uuid myModelSamplersUuid; // the loaded model's samplers, see InstallModel. nil until one is loaded
	uuids::uuid myMaterialsUuid;
	// gMaterialData's size (in materials), which grows to hold the installed model's (see GrowMaterials), and material 0
	// (the default, textured by an opened image), which a grown buffer starts with
	uint32_t myMaterialCapacity = SHADER_TYPES_MATERIAL_COUNT;
	MaterialData myDefaultMaterial = DefaultMaterialData();
	uuids::uuid myTextureViewsUuid;
	uuids::uuid myModelInstancesUuid;
	// bound as gSkinVertices, gJointMatrices, gMorphDeltas and gMorphWeights while the installed model has none (a model
	// that doesn't move, isn't skinned or has no animated morph targets): one zero skin vertex, one identity joint matrix,
	// one zero morph delta and one zero weight
	uuids::uuid myDefaultSkinVerticesUuid;
	uuids::uuid myDefaultJointsUuid;
	uuids::uuid myDefaultMorphDeltasUuid;
	uuids::uuid myDefaultMorphWeightsUuid;
	// bound as gBvhNodes and gBvhTriangles while the installed model has no BVH (see gfx/bvh.h): a node's worth of zeros,
	// which GiData::trace never has the rays traverse, and as gEmitters and gEmitterTree while it has no emitters (see
	// gfx/emitters.h: EmitterData::count is 0 then)
	uuids::uuid myDefaultBvhUuid;
	// bound as gBvhScene while the installed model has nothing the device's ray queries trace (see AccelerationStructures):
	// top level structures of no instances. null where the device has no ray queries
	std::unique_ptr<AccelerationStructures> myDefaultAccelerationStructures;
	// diagnostic, bound as gRayQueryWatch (SPEEDO_RAY_QUERY_WATCH=1, see gfx/rayquerywatch.h): null without it
	std::unique_ptr<RayQueryWatch> myRayQueryWatch;
	bool myMovingPlaced = false; // whether this frame's Update placed what moves, for the rays (see Model::Animate)
	// what the last Update posed the model with (the model, its pose, the one crossfaded from and the weight), and whether
	// this frame's is the same: what may move is then where the last frame had it (see FrameInputs::modelStill)
	struct PosedAs
	{
		const Model* model = nullptr;
		std::optional<size_t> animation;
		float time = 0.0F;
		std::optional<size_t> previous;
		float previousTime = 0.0F;
		float weight = 1.0F;
		bool operator==(const PosedAs&) const = default;
	};
	std::optional<PosedAs> myPosedAs;
	bool myModelStill = false;
	core::ConcurrentAccess<AnimationState> myAnimation;
	core::ConcurrentAccess<SceneState> myScenes;
	// gLights (SHADER_TYPES_LIGHT_COUNT of them): the installed model's, or the default light. draw thread.
	uuids::uuid myLightsUuid;
	uint32_t myLightCount = 0;
	std::vector<LightData> myLightData; // gLights' contents, for the shadows (see Renderer)
	// what gLights is made of (see SyncLights): the installed model's lights (draw thread), and the installed environment's
	// dominant lights (draw thread: nullopt until an environment is installed). per dominant light, its irradiance (lux,
	// luminance) and whether the model has the same light, for the ui
	std::vector<LightData> mySceneLightData;
	std::optional<std::vector<environment::DominantLight>> myEnvironmentLights;
	core::ConcurrentAccess<std::vector<DominantLightInfo>> myEnvironmentLightInfo;
	core::ConcurrentAccess<std::vector<float>> myLightSizes; // see Scene::GetLightSizes
	// per view (split screen's, each by its own pixels): written by the draw thread, shown by the ui (the first's), and whether
	// the view has been measured yet (its first measure is taken as it is)
	std::array<std::atomic<float>, SHADER_TYPES_SPLIT_VIEW_COUNT> myAutoExposureStops{};
	std::array<bool, SHADER_TYPES_SPLIT_VIEW_COUNT> myAutoExposureMeasured{};
	// how much each view's scene is seen by the eye's night vision (see ViewData::scotopic), by its luminance, adapted as
	// the exposure is
	std::array<std::atomic<float>, SHADER_TYPES_SPLIT_VIEW_COUNT> myScotopic{};
	std::array<bool, SHADER_TYPES_SPLIT_VIEW_COUNT> myScotopicMeasured{};
	// the upscaling the render targets were last sized for (Settings::upscaling, see CreateOutputDependentObjects). the
	// draw thread's
	Upscaling myUpscaling = Upscaling::kOff;
	std::optional<std::chrono::steady_clock::time_point> myAutoExposureLast;
	uint64_t myExposureLogFrame = 0; // SPEEDO_EXPOSURE_LOG's frame count
	// how far each view looks (see FrameInputs::lookDistances), by the draw thread, and whether it has been measured yet
	std::array<std::atomic<float>, SHADER_TYPES_SPLIT_VIEW_COUNT> myLookDistances{};
	std::array<bool, SHADER_TYPES_SPLIT_VIEW_COUNT> myLookDistanceMeasured{};
	// and how far what is under each view's center spot and center zone is (see SHADER_TYPES_FOCUS_BINS), and the
	// nearest tenth of its pixels, the autofocus modes', likewise, and the focus held where it isn't continuous (meters)
	std::array<std::atomic<float>, SHADER_TYPES_SPLIT_VIEW_COUNT> myFocusDistances{};
	std::array<bool, SHADER_TYPES_SPLIT_VIEW_COUNT> myFocusMeasured{};
	std::array<std::atomic<float>, SHADER_TYPES_SPLIT_VIEW_COUNT> myZoneDistances{};
	std::array<bool, SHADER_TYPES_SPLIT_VIEW_COUNT> myZoneMeasured{};
	std::array<std::atomic<float>, SHADER_TYPES_SPLIT_VIEW_COUNT> myNearDistances{};
	std::array<bool, SHADER_TYPES_SPLIT_VIEW_COUNT> myNearMeasured{};
	float myFocusHeld = 0.0F;
	uuids::uuid myExposureHistogramUuid; // gExposureHistogram: SHADER_TYPES_FRAME_HISTOGRAM_BINS per frame, host visible
	size_t myModelSamplerCount = 0; // how many of kModelSamplerSlots the loaded model uses
	// the camera's lens as last appeared (see lens::Appear, FrameLens) and what it appeared for (the lens, the aperture to a
	// sixth of a stop, the sensor's height and the first view's aspect, which give the sensor's half diagonal: made again
	// when any changes), and the margin the frame is rendered wider by for it (see Views::SetLens, LensData::margin). the
	// draw thread's
	struct LensState
	{
		uint32_t lens = std::numeric_limits<uint32_t>::max();
		int apertureSixths = 0;
		float sensorHeight = 0.0F;
		float aspect = 0.0F;
		float halfDiagonal = 0.0F;
		lens::Appearance appearance;
		float margin = 1.0F;
		// the glare's (see LensGhost, LensData::ghostGain and starGain)
		std::vector<LensGhost> ghosts;
		float ghostGain = 0.0F;
		float starGain = 0.0F;
	};
	LensState myLensState;
	// the frames' length (seconds, smoothed over about ten, see UpdateSky: SPEEDO_TIME_FRAME_SECONDS's where set), which
	// the motion blur's shutter is a share of, and the frames the lens has appeared for (the grain's seed). the draw thread's
	double myFrameSeconds = 1.0 / 60.0;
	uint32_t myLensFrame = 0;


	// the loaded model's textures, by slot (from kModelTextureFirstSlot). nil for slots it doesn't use.
	std::vector<std::pair<uuids::uuid, uuids::uuid>> myModelTextureUuids; // image, view
	std::vector<MaterialData> myModelMaterialData;
	std::vector<AnimatedTextureView> myAnimatedTextureViews;
	std::vector<SceneLight> myAnimatedLights; // the lights as last uploaded, if some follow nodes


	// the exposure the camera exposes a view with (see Settings::exposureMode, camera::Choose): what is set, or what it
	// chooses in its mode for the stops auto exposure asks for
	[[nodiscard]] camera::Exposure ChosenExposure(size_t view) const
	{
		auto mode = static_cast<camera::ExposureMode>(mySettings.exposureMode.load(std::memory_order_relaxed));
		camera::Exposure set{
			.aperture = mySettings.aperture.load(std::memory_order_relaxed),
			.shutter = mySettings.shutter.load(std::memory_order_relaxed),
			.iso = mySettings.iso.load(std::memory_order_relaxed)};
		float stops = view < myAutoExposureStops.size() ? myAutoExposureStops[view].load(std::memory_order_relaxed) : 0.0F;
		return camera::Choose(mode, stops, set, LensLimits());
	}

	// the limits the camera exposes within: the lens's aperture range (see lens::Prescription; the pinhole's any), the rest
	// the defaults
	[[nodiscard]] camera::ExposureLimits LensLimits() const noexcept
	{
		camera::ExposureLimits limits{};
		const auto& lenses = lens::Lenses();
		auto index = mySettings.lens.load(std::memory_order_relaxed);
		if (index < lenses.size() && !lens::IsPinhole(lenses[index]))
		{
			limits.widestAperture = lenses[index].widestAperture;
			limits.narrowestAperture = lenses[index].narrowestAperture;
		}
		return limits;
	}

	// the camera's lens this frame (see LensData, lens::Appear): the appearance made again where what it depends on changed
	// (see LensState), and the views given the lens's focal length and margin then (see Views::SetLens: before they upload,
	// so that the frame renders the field the lens shows); the pinhole's nothing, the Lens pass disabled. call on the draw
	// thread, recording a frame, before views.Upload
	[[nodiscard]] LensData FrameLens(Views& views)
	{
		const auto& lenses = lens::Lenses();
		auto index = mySettings.lens.load(std::memory_order_relaxed);
		if (index >= lenses.size() || lens::IsPinhole(lenses[index]))
		{
			if (myLensState.lens != index)
			{
				myLensState = LensState{.lens = index};
				views.SetLens(std::nullopt, 1.0F);
			}
			return LensData{};
		}
		const auto& prescription = lenses[index];
		// the aperture to a sixth of a stop (the vignetting changes smoothly with it, and the aperture every frame under
		// auto exposure), and the sensor's half diagonal: its height and the first view's aspect (split screen's views
		// share one), which an anamorphic lens's image is the squeeze wider than (see Views::SetLens)
		auto aperture = ChosenExposure(0).aperture;
		auto apertureSixths = static_cast<int>(std::lround(6.0 * std::log2(static_cast<double>(aperture) * static_cast<double>(aperture))));
		auto viewports = views.GetViewports();
		float aspect = viewports.empty() || viewports.front().height == 0 ? 1.0F : static_cast<float>(viewports.front().width) / static_cast<float>(viewports.front().height);
		if (views.GetLensSqueeze() == prescription.squeeze)
			aspect /= prescription.squeeze; // the sensor's, which the views are letterboxed the squeeze wider than
		auto sensorHeight = views.GetSensorHeight();
		if (myLensState.lens != index || myLensState.apertureSixths != apertureSixths || myLensState.sensorHeight != sensorHeight || myLensState.aspect != aspect)
		{
			myLensState.lens = index;
			myLensState.apertureSixths = apertureSixths;
			myLensState.sensorHeight = sensorHeight;
			myLensState.aspect = aspect;
			myLensState.halfDiagonal = 0.5F * sensorHeight * std::sqrt(1.0F + aspect * aspect);
			auto quantized = static_cast<float>(std::exp2(static_cast<double>(apertureSixths) / 12.0));
			myLensState.appearance = lens::Appear(prescription, quantized, myLensState.halfDiagonal);
			// the frame rendered wider than the lens's image by what reads beyond it: the ideal image's radius at the lens's
			// corner under barrel distortion, 1 - k, and the red or the blue beyond the green's (see LensSample)
			const auto& appearance = myLensState.appearance;
			auto leastScale = std::clamp(std::min({1.0F, appearance.redScale, appearance.blueScale}), 0.5F, 1.0F);
			myLensState.margin = (1.0F - std::min(prescription.distortion, 0.0F)) / leastScale;
			views.SetLens(prescription.focalLength, myLensState.margin, prescription.squeeze);
			// the ghosts (see LensGhost: a, b to the sensor and as, bs to the stop per channel, and the reflectances) and
			// the gains the glare's sources are worth drawing by (see ComputeFlareSources): a ghost's irradiance over the
			// source's, the brightest's, as radiance (f^2 over pi times the pupil's radius squared), and the starburst's at
			// a millimeter (see FlareStarburst: the blades' edge squared times the f-number squared over pi^3, times its
			// width over a gaussian's integral at least a pixel wide)
			auto ghosts = lens::Ghosts(prescription, quantized);
			ENSUREF(
				ghosts.size() <= SHADER_TYPES_LENS_GHOST_COUNT,
				"{} has {} ghosts, more than SHADER_TYPES_LENS_GHOST_COUNT ({})",
				prescription.name,
				ghosts.size(),
				SHADER_TYPES_LENS_GHOST_COUNT);
			auto pupilRadius = std::max(appearance.entrancePupilRadius, 1e-3F);
			auto toRadiance = appearance.focalLength * appearance.focalLength / (glm::pi<float>() * pupilRadius * pupilRadius);
			myLensState.ghosts.clear();
			myLensState.ghostGain = 0.0F;
			for (const auto& ghost : ghosts)
			{
				LensGhost data{};
				for (glm::length_t channel = 0; channel < 3; channel++)
				{
					const auto& toSensor = ghost.toSensor[static_cast<size_t>(channel)];
					const auto& toStop = ghost.toStop[static_cast<size_t>(channel)];
					data.sensor[channel][0] = toSensor[0][0];
					data.sensor[channel][1] = toSensor[1][0];
					data.sensor[channel][2] = toStop[0][0];
					data.sensor[channel][3] = toStop[1][0];
					data.reflectance[channel] = ghost.reflectance[channel];
					auto magnification = std::max(toSensor[0][0] * toSensor[0][0], 1e-6F);
					myLensState.ghostGain = std::max(myLensState.ghostGain, ghost.reflectance[channel] / magnification * toRadiance);
				}
				myLensState.ghosts.push_back(data);
			}
			auto fNumber = appearance.focalLength / (2.0F * pupilRadius);
			auto edge = prescription.bladeCount >= 3 ? 2.0F * appearance.stopRadius * std::sin(glm::pi<float>() / static_cast<float>(prescription.bladeCount)) : 0.0F;
			auto pixelMm = sensorHeight / static_cast<float>(std::max<uint32_t>(viewports.empty() ? 1 : viewports.front().height, 1));
			constexpr float kGreenWavelengthMm = 550e-6F;
			auto width = kGreenWavelengthMm * appearance.focalLength / std::max(edge, 1e-6F);
			auto sigma = std::max(width / 2.5F, 0.6F * pixelMm);
			myLensState.starGain = edge > 0.0F ? edge * edge * fNumber * fNumber / std::pow(glm::pi<float>(), 3.0F) * width / (sigma * std::sqrt(2.0F * glm::pi<float>())) : 0.0F;
		}
		const auto& appearance = myLensState.appearance;
		bool glare = mySettings.lensGlare.load(std::memory_order_relaxed);
		LensData data{};
		for (size_t sampleIt = 0; sampleIt < lens::Appearance::kVignettingSamples; sampleIt++)
			data.vignetting[sampleIt / 4][sampleIt % 4] = appearance.vignetting[sampleIt];
		data.fieldAngle = appearance.fieldAngle;
		data.focalLength = appearance.focalLength;
		data.halfDiagonal = myLensState.halfDiagonal;
		data.redScale = appearance.redScale;
		data.blueScale = appearance.blueScale;
		data.distortion = prescription.distortion;
		data.margin = myLensState.margin;
		data.transmittance = appearance.transmittance;
		data.squeeze = prescription.squeeze;
		data.enabled = 1U;
		data.bladeCount = prescription.bladeCount;
		data.ghostCount = glare ? static_cast<uint32_t>(myLensState.ghosts.size()) : 0U;
		data.glare = glare ? appearance.glare : 0.0F;
		data.pupilRadius = appearance.entrancePupilRadius;
		data.stopRadius = appearance.stopRadius;
		data.entranceRadius = prescription.surfaces.empty() ? 0.0F : prescription.surfaces.front().clearRadius;
		data.ghostGain = myLensState.ghostGain;
		data.starGain = myLensState.starGain;
		data.flares = glare ? 1U : 0U;
		for (size_t sampleIt = 0; sampleIt < lens::Appearance::kVignettingSamples; sampleIt++)
			data.catEye[sampleIt / 4][sampleIt % 4] = appearance.catEye[sampleIt];
		static_assert(lens::Appearance::kBokehSamples == lens::Appearance::kVignettingSamples);
		for (size_t sampleIt = 0; sampleIt < lens::Appearance::kBokehSamples; sampleIt++)
		{
			data.bokehNear[sampleIt / 4][sampleIt % 4] = appearance.bokehNear[sampleIt];
			data.bokehFar[sampleIt / 4][sampleIt % 4] = appearance.bokehFar[sampleIt];
		}
		data.bokehReference = appearance.bokehReference;
		// the depth of field: the focus in mm along the view (the view's own measure in the scene's units, or the setting in
		// meters), the aperture's diameter at the aperture the camera exposes with (not quantized: a stop's width of focus
		// would step)
		data.dof = 1U;
		data.focusDistance = FocusDistanceMeters() * 1000.0F;
		data.apertureDiameter = appearance.focalLength / std::max(aperture, 0.1F);
		data.focusShiftRed = appearance.focalLengthRed - appearance.focalLength;
		data.focusShiftBlue = appearance.focalLengthBlue - appearance.focalLength;
		// the motion blur over the shutter's share of the frame (a share of the frame's motion: at most a few frames'),
		// and the grain: the shot noise of a white pixel, the square root of the photons a full well holds at ISO 100 (a
		// sensor's twenty thousand electrons, kGrainFullWell) over the gain
		auto exposure = ChosenExposure(0);
		data.motionBlur = mySettings.motionBlur.load(std::memory_order_relaxed) ? 1U : 0U;
		data.shutterShare = static_cast<float>(std::clamp(static_cast<double>(exposure.shutter) / std::max(myFrameSeconds, 1e-3), 0.0, 4.0));
		constexpr float kGrainFullWell = 20000.0F;
		data.grain = mySettings.grain.load(std::memory_order_relaxed) ? 1U : 0U;
		data.grainNoise = std::sqrt(std::max(exposure.iso, 1.0F) / 100.0F / kGrainFullWell);
		data.grainFrame = myLensFrame++;
		return data;
	}

	// what the first view's autofocus measures in its mode (see Settings::focusMode), in the scene's units: under the
	// center spot, the center zone, or the nearest thing in view (until measured, how far the view looks)
	[[nodiscard]] float MeasuredFocusUnits() const
	{
		auto mode = static_cast<FocusMode>(mySettings.focusMode.load(std::memory_order_relaxed));
		switch (mode)
		{
		case FocusMode::kSpot:
			if (myFocusMeasured[0])
				return myFocusDistances[0].load(std::memory_order_relaxed);
			break;
		case FocusMode::kZone:
			if (myZoneMeasured[0])
				return myZoneDistances[0].load(std::memory_order_relaxed);
			break;
		case FocusMode::kWide:
			if (myNearMeasured[0])
				return myNearDistances[0].load(std::memory_order_relaxed);
			break;
		default: break;
		}
		return myLookDistances[0].load(std::memory_order_relaxed);
	}

	// what the camera focuses at, meters: the setting in manual, else the autofocus's measure, followed continuously or
	// held as last acquired (Settings::focusContinuous, focusAcquire). draw thread
	[[nodiscard]] float FocusDistanceMeters()
	{
		auto mode = static_cast<FocusMode>(mySettings.focusMode.load(std::memory_order_relaxed));
		if (mode == FocusMode::kManual)
			return std::max(mySettings.focusDistance.load(std::memory_order_relaxed), 1e-3F);
		auto measured = MeasuredFocusUnits() * mySettings.metersPerUnit.load(std::memory_order_relaxed);
		if (mySettings.focusContinuous.load(std::memory_order_relaxed) || mySettings.focusAcquire.exchange(false, std::memory_order_relaxed) || myFocusHeld <= 0.0F)
			myFocusHeld = measured;
		return std::max(myFocusHeld, 1e-3F);
	}

	// a view's exposure, as a factor: the camera's exposure in stops (see ChosenExposure) and the compensation (see
	// ViewData::exposure)
	float ViewExposure(size_t view) const
	{
		return std::exp2(mySettings.exposureStops.load(std::memory_order_relaxed) + ChosenExposure(view).Stops());
	}
	float ViewScotopic(size_t view) const { return view < myScotopic.size() ? myScotopic[view].load(std::memory_order_relaxed) : 0.0F; }

	// the push constants every draw (and ComputeMain) of a frame shares
	PushConstants FramePushConstants(uint16_t frameIndex, uint16_t outputIndex)
	{
		return PushConstants{
			.frameIndex = frameIndex,
			.lightCount = myLightCount,
			.exposure = ViewExposure(0),
			.environmentIntensity = mySettings.environmentIntensity.load(std::memory_order_relaxed),
			.environmentRotation = glm::radians(mySettings.environmentRotationDegrees.load(std::memory_order_relaxed)),
			.viewCount = myViews.GetGrid().x * myViews.GetGrid().y, // at most SHADER_TYPES_VIEW_COUNT (see Views::Upload)
			.environmentBackdrop = mySettings.environmentBackdrop.load(std::memory_order_relaxed) ? 1U : 0U,
			.tonemapper = mySettings.tonemapper.load(std::memory_order_relaxed),
			.shadowView = SHADER_TYPES_NO_SHADOW,
			.debugView = std::min(mySettings.debugView.load(std::memory_order_relaxed), DEBUG_VIEW_COUNT - 1),
			.outputIndex = outputIndex,
			.metersPerUnit = std::max(mySettings.metersPerUnit.load(std::memory_order_relaxed), 1e-6F)};
	}


	// writes materials, starting at slot first (see UpdateBufferOnGraphics). call on the draw thread.
	void UpdateMaterials(
		RHI& rhi, QueueTimelineContextData& graphics, uint32_t first, std::span<const MaterialData> materials)
	{
		ENSURE(first + materials.size() <= myMaterialCapacity);

		UpdateBufferOnGraphics(
			graphics, *rhi.GetPrimaryDevice().GetResource<Buffer>(myMaterialsUuid), first * sizeof(MaterialData), std::as_bytes(materials));
	}


	// makes gMaterialData hold count materials: if it is smaller, a new buffer of the next power of two, the default
	// materials in it (material 0 as it is), bound in its place, and the old one retired once the gpu is done with it
	// (NodePerformanceTest has 10000 materials per model). call on the draw thread, before writing the materials
	void GrowMaterials(RHI& rhi, QueueTimelineContextData& graphics, size_t count)
	{
		if (count <= myMaterialCapacity)
			return;

		auto& device = rhi.GetPrimaryDevice();
		auto capacity = static_cast<uint32_t>(std::bit_ceil(count));
		std::vector<MaterialData> data(capacity, DefaultMaterialData());
		data.front() = myDefaultMaterial;
		auto buffer = std::make_shared<Buffer>(BufferCreateDesc{
			device.CreateDeviceObjectCreateDesc("Materials"),
			capacity * sizeof(MaterialData),
			BufferUsage::kStorage | BufferUsage::kTransferDestination, // UpdateMaterials writes it with UpdateBuffer
			MemoryProperty::kHostVisible});
		auto bytes = std::as_bytes(std::span(data));
		auto memory = buffer->Map();
		std::ranges::copy(bytes, memory.begin());
		buffer->Flush(0, bytes.size());
		buffer->Unmap();

		myPipeline.SetDescriptorData("gMaterialData", BufferBinding{.buffer = *buffer, .offset = 0}, DESCRIPTOR_SET_CATEGORY_MATERIAL);
		RetireAfterGraphicsWork(graphics, device.ReplaceResource(myMaterialsUuid, buffer));
		myMaterialsUuid = buffer->GetUuid();
		myMaterialCapacity = capacity;
	}

	// writes a model's lights to gLights, or the default light if it has none: a directional light from above that, with
	// the ambient light in the shader, lights matte surfaces as before there were lights. call on the draw thread.
	// sets the installed model's lights (none for a model without), for SyncLights to upload. call on the draw thread.
	void UpdateLights(std::span<const SceneLight> sceneLights)
	{
		// the rest are the environment's dominant lights
		constexpr size_t kModelLightCount = SHADER_TYPES_LIGHT_COUNT - environment::kMaxDominantLights;
		ENSUREF(
			sceneLights.size() <= kModelLightCount,
			"{} lights, more than the {} a model may have (SHADER_TYPES_LIGHT_COUNT)",
			sceneLights.size(),
			kModelLightCount);

		std::vector<LightData> lights(sceneLights.size());
		for (size_t lightIt = 0; lightIt < lights.size(); lightIt++)
		{
			const auto& sceneLight = sceneLights[lightIt];
			auto& light = lights[lightIt];
			std::copy_n(glm::value_ptr(sceneLight.position), 3, light.positionRange);
			light.positionRange[3] = sceneLight.range;
			std::copy_n(glm::value_ptr(sceneLight.direction), 3, light.direction);
			auto intensity = sceneLight.color * sceneLight.intensity;
			std::copy_n(glm::value_ptr(intensity), 3, light.intensity);
			light.type = sceneLight.type == SceneLight::Type::kPoint  ? LIGHT_TYPE_POINT
					   : sceneLight.type == SceneLight::Type::kSpot ? LIGHT_TYPE_SPOT
																	  : LIGHT_TYPE_DIRECTIONAL;
			// KHR_lights_punctual's cone attenuation
			auto cosOuter = std::cos(sceneLight.outerConeAngle);
			light.spotScale = 1.0F / std::max(0.001F, std::cos(sceneLight.innerConeAngle) - cosOuter);
			light.spotOffset = -cosOuter * light.spotScale;
		}
		mySceneLightData = std::move(lights);
	}


	// toward the sky's sun, where the time of day puts it (in the panorama's space, see environment::SunDirection)
	glm::vec3 SkySunDirection() const
	{
		return environment::SunDirection(
			mySettings.latitude.load(std::memory_order_relaxed),
			mySettings.dayOfYear.load(std::memory_order_relaxed),
			mySettings.timeOfDay.load(std::memory_order_relaxed));
	}

	// how much of the aerosols the sky's air has, by the visibility through it (see environment::SkyAerosols)
	float SkyAerosols() const { return environment::SkyAerosols(mySettings.visibility.load(std::memory_order_relaxed)); }

	// the sky as the time, the place and the air put it: toward the sun (see SkySunDirection) and the moon, its phase and
	// illuminance (environment::MoonAt), the local sidereal time and the latitude (radians, the stars' frame) and the
	// aerosols: what gEnvironment's sky fields, the sky's lights and its redraws go by
	struct SkyState
	{
		glm::vec3 sun{0.0F, 1.0F, 0.0F};
		environment::Moon moon;
		float sidereal = 0.0F;
		float latitude = 0.0F;
		float aerosols = 1.0F;
	};
	SkyState SkyNow() const
	{
		float latitude = mySettings.latitude.load(std::memory_order_relaxed);
		float day = mySettings.dayOfYear.load(std::memory_order_relaxed);
		float time = mySettings.timeOfDay.load(std::memory_order_relaxed);
		int year = mySettings.year.load(std::memory_order_relaxed);
		return {
			.sun = environment::SunDirection(latitude, day, time),
			.moon = environment::MoonAt(latitude, day, time, year),
			.sidereal = environment::LocalSiderealTime(day, time, year),
			.latitude = glm::radians(latitude),
			.aerosols = SkyAerosols()};
	}

	// the sky's dominant lights through its air: the sun as a directional light while it is above the horizon, else the
	// moon while it is (by day its light is a millionth of the sun's, not worth its shadow views and traced rays)
	static std::vector<environment::DominantLight> SkyLights(const SkyState& sky)
	{
		if (auto sun = environment::SkySun(sky.sun, sky.aerosols))
			return {sun};
		if (auto moon = environment::SkyMoon(sky.moon, sky.aerosols))
			return {moon};
		return {};
	}

	// gEnvironment's sky fields (see EnvironmentData::skySun): the sun's and the moon's disks' radiance, their illuminance
	// through the atmosphere over their solid angle (the moon's over the share of a lit sphere's light its phase shows, as
	// SkyMoonDisk draws it: a Lambertian sphere's, 2/3 at full), the aerosols, and the stars' frame and the night's glow
	static void FillSkyData(EnvironmentData& data, const SkyState& sky)
	{
		std::copy_n(glm::value_ptr(sky.sun), 3, data.skySun);
		float cosRadius = std::cos(environment::kSkySunRadius);
		float solidAngle = 2.0F * glm::pi<float>() * (1.0F - cosRadius);
		data.skySun[3] = cosRadius;
		auto radiance = environment::SkySun(sky.sun, sky.aerosols).irradiance / solidAngle;
		std::copy_n(glm::value_ptr(radiance), 3, data.skySunRadiance);
		data.skySunRadiance[3] = 1.0F;
		data.skyAtmosphere[0] = sky.aerosols;
		std::copy_n(glm::value_ptr(sky.moon.direction), 3, data.skyMoon);
		data.skyMoon[3] = cosRadius;
		// a Lambertian sphere's phase law: the share of its full light at an elongation e from the sun (a phase angle of pi
		// less e) is (sin e - e cos e) / pi, and its full disk's mean cosine is 2/3
		float elongation = sky.moon.elongation;
		float litShare = std::max(2.0F / 3.0F * (std::sin(elongation) - (elongation * std::cos(elongation))) / glm::pi<float>(), 0.01F);
		auto moonRadiance = environment::SkyMoon(sky.moon, sky.aerosols).irradiance / (solidAngle * litShare);
		std::copy_n(glm::value_ptr(moonRadiance), 3, data.skyMoonRadiance);
		data.skyMoonRadiance[3] = sky.moon.illuminance;
		data.skyNight[0] = sky.sidereal;
		data.skyNight[1] = sky.latitude;
		data.skyNight[2] = static_cast<float>(SHADER_TYPES_ATMOSPHERE_NIGHT_RADIANCE);
		data.skyNight[3] = 0.0F;
	}

	// a model's directional light within this (degrees) of a dominant light's direction is the same light (see SyncLights)
	static constexpr float kMergeAngle = 5.0F;

	// toward the sky's sun as last drawn, in the world (turned back by the environment's rotation, as SyncLights turns the
	// dominant lights), which the aerial perspective is lit by
	glm::vec3 SkySunWorld() const { return SkyWorld(mySkySunDrawn); }

	// which of the lights (gLights) is the sky's sun toward sun: its own dominant light, or a model's directional light
	// merged with it (the nearest within kMergeAngle), if any (none below the horizon), for the light shafts' shadow views
	std::optional<uint32_t> SkySunLight(const glm::vec3& sun) const
	{
		std::optional<uint32_t> found;
		float best = std::cos(glm::radians(kMergeAngle));
		for (size_t lightIt = 0; lightIt < myLightData.size(); lightIt++)
		{
			const auto& light = myLightData[lightIt];
			glm::vec3 toward(-light.direction[0], -light.direction[1], -light.direction[2]);
			if (light.type != LIGHT_TYPE_DIRECTIONAL || !(glm::length(toward) > 0.0F))
				continue;
			if (float cosine = glm::dot(glm::normalize(toward), sun); cosine >= best)
			{
				best = cosine;
				found = static_cast<uint32_t>(lightIt);
			}
		}
		return found;
	}

	// draws the sky again where the sun or the moon has moved more than this (degrees) since it was last drawn (see
	// UpdateSky): a still sun's minute at real time, so that a time advancing slowly draws it every few seconds, and fast,
	// every frame
	static constexpr float kSkyRedrawAngle = 0.05F;
	// and uploads gEnvironment's sky fields alone (the disks' places, the stars' frame) where either has moved more than
	// this: a tenth of it, under a pixel, so that the sun, the moon and the stars move smoothly between the redraws (the
	// stars stepped 3 pixels every 12 seconds at real time)
	static constexpr float kSkyUpdateAngle = 0.005F;

	// the sky's directions in the world: the panorama's space turned by the environment's rotation
	glm::vec3 SkyWorld(const glm::vec3& d) const
	{
		float angle = glm::radians(mySettings.environmentRotationDegrees.load(std::memory_order_relaxed));
		float c = std::cos(angle);
		float s = std::sin(angle);
		return {(c * d.x) + (s * d.z), d.y, (-s * d.x) + (c * d.z)};
	}

	// the light the sky's air is lit by, as the aerial perspective marches it (FrameInputs::airLight): toward it, in the
	// world, and its illuminance at the top of the atmosphere; the sun while it is above the horizon, else the moon, whose
	// light the sun's twilight outshines until the sun sets (the switch changes nothing to see)
	glm::vec4 SkyAirLight() const
	{
		if (mySkySunUpDrawn)
			return {SkyWorld(mySkySunDrawn), static_cast<float>(SHADER_TYPES_ATMOSPHERE_SUN_ILLUMINANCE)};
		return {SkyWorld(mySkyMoonDrawn), mySkyMoonIlluminanceDrawn};
	}

	// the time of day advanced (Settings::advanceTime) by the real time since the last frame, and the sky drawn again for
	// its sun when it has moved (see kSkyRedrawAngle): gEnvironment's sky fields, its view of itself, its panorama and the
	// prefiltering, before the frame (the same queue), into the installed environment, and its dominant lights. draw thread.
	void UpdateSky(RHI& rhi, QueueTimelineContextData& graphics)
	{
		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration<double>(now - myTimeLast).count();
		myTimeLast = now;
		// for comparing runs: as many seconds a drawn frame once a model is in (SPEEDO_TIME_FRAME_SECONDS), so that two runs
		// have the sun where it was at the same frame, whatever their loads and frames took
		static const double kFrameSeconds = []
		{
			const char* seconds = std::getenv("SPEEDO_TIME_FRAME_SECONDS");
			return seconds != nullptr && *seconds != '\0' ? std::max(std::strtod(seconds, nullptr), 0.0) : -1.0;
		}();
		if (kFrameSeconds >= 0.0)
			elapsed = myModel ? kFrameSeconds : 0.0;
		// the frames' length, for the motion blur's shutter (smoothed; the fixed one as it is)
		if (elapsed > 0.0)
			myFrameSeconds = kFrameSeconds >= 0.0 ? elapsed : std::clamp(myFrameSeconds + 0.1 * (elapsed - myFrameSeconds), 1e-3, 1.0);
		float speed = mySettings.timeSpeed.load(std::memory_order_relaxed);
		if (mySettings.advanceTime.load(std::memory_order_relaxed) && speed != 0.0F)
		{
			double hours = mySettings.timeOfDay.load(std::memory_order_relaxed) + (elapsed * speed / 3600.0);
			hours = std::fmod(hours, 24.0);
			mySettings.timeOfDay.store(static_cast<float>(hours < 0.0 ? hours + 24.0 : hours), std::memory_order_relaxed);
		}
		if (!mySky)
			return;
		auto sky = SkyNow();
		float redraw = std::cos(glm::radians(kSkyRedrawAngle));
		float update = std::cos(glm::radians(kSkyUpdateAngle));
		bool redrawTables = glm::dot(sky.sun, mySkySunDrawn) < redraw || glm::dot(sky.moon.direction, mySkyMoonDrawn) < redraw ||
							sky.aerosols != mySkyAerosolsDrawn;
		if (!redrawTables && glm::dot(sky.sun, mySkySunUploaded) >= update && glm::dot(sky.moon.direction, mySkyMoonUploaded) >= update)
			return;

		auto& device = rhi.GetPrimaryDevice();
		auto& environmentBuffer = *device.GetResource<Buffer>(myEnvironmentUuid);
		EnvironmentData data{};
		FillSkyData(data, sky);
		// the sky fields alone: the irradiance is the filter's to write
		static_assert(offsetof(EnvironmentData, skySunRadiance) == offsetof(EnvironmentData, skySun) + sizeof(data.skySun));
		static_assert(offsetof(EnvironmentData, skyAtmosphere) == offsetof(EnvironmentData, skySunRadiance) + sizeof(data.skySunRadiance));
		static_assert(offsetof(EnvironmentData, skyMoon) == offsetof(EnvironmentData, skyAtmosphere) + sizeof(data.skyAtmosphere));
		static_assert(offsetof(EnvironmentData, skyMoonRadiance) == offsetof(EnvironmentData, skyMoon) + sizeof(data.skyMoon));
		static_assert(offsetof(EnvironmentData, skyNight) == offsetof(EnvironmentData, skyMoonRadiance) + sizeof(data.skyMoonRadiance));
		auto first = reinterpret_cast<const std::byte*>(&data) + offsetof(EnvironmentData, skySun);
		std::span fields(first, offsetof(EnvironmentData, skyNight) + sizeof(data.skyNight) - offsetof(EnvironmentData, skySun));
		mySkySunUploaded = sky.sun;
		mySkyMoonUploaded = sky.moon.direction;
		// the fields and the redraw in one command buffer (a submit each cost a time lapse of 600x about half a millisecond
		// a frame on KosmicKrisp); the sky's lights move with the tables (every kSkyRedrawAngle, not with the fields): a
		// change of the lights redraws the sun's shadow cascades and relights the static voxels, which a 0.05 degree step
		// doesn't show (a shadow's tip moves a centimetre at 10 m), and with the fields a time lapse of 60x paid them every
		// frame rather than every 12th
		std::shared_ptr<void> used;
		RecordOnGraphics(
			graphics,
			[&](CommandBufferHandle cmd)
			{
				RecordBufferUpdate(cmd, environmentBuffer, offsetof(EnvironmentData, skySun), fields);
				if (redrawTables)
					used = RecordEnvironment(cmd, graphics, *mySky, environmentBuffer, sky.aerosols);
			});
		if (!redrawTables)
			return;
		RetireAfterGraphicsWork(graphics, std::move(used));
		myEnvironmentLights = SkyLights(sky);
		RememberSkyDrawn(sky);
		// the frame binds what it draws with: the main layout, as before
		myPipeline.BindLayoutAuto(myPipeline.GetLayoutHandle("Main"), PipelineBindPoint::kGraphics);
	}

	// what the sky was drawn for (see UpdateSky)
	void RememberSkyDrawn(const SkyState& sky)
	{
		mySkySunDrawn = sky.sun;
		mySkyMoonDrawn = sky.moon.direction;
		mySkyAerosolsDrawn = sky.aerosols;
		mySkyMoonIlluminanceDrawn = sky.moon.illuminance;
		mySkySunUpDrawn = static_cast<bool>(environment::SkySun(sky.sun, sky.aerosols));
		mySkySunUploaded = sky.sun;
		mySkyMoonUploaded = sky.moon.direction;
	}

	// prefilters an environment on the gpu (see EnvironmentFilter::Record), before the frames that sample it (the same
	// queue), keeping what it reads until the gpu is done. draw thread.
	// records an environment's prefiltering (see EnvironmentFilter::Record) into cmd, a command buffer of the graphics
	// queue's; returns what the commands use, to retire once the submit they are in is done
	[[nodiscard]] std::shared_ptr<void> RecordEnvironment(
		CommandBufferHandle cmd,
		QueueTimelineContextData& graphics,
		const EnvironmentTexture& environment,
		const Buffer& environmentBuffer,
		float skyAerosols = 1.0F)
	{
		auto& [graphicsQueue, graphicsSubmits] = graphics.queues.Get();
		return myEnvironmentFilter->Record(
			cmd, myPipeline, environment, environmentBuffer, skyAerosols, graphicsQueue.GetProfiler(), myRenderer ? myRenderer->GetTimingGroup() : 0);
	}

	// the same in a submit of its own
	void RecordEnvironment(
		QueueTimelineContextData& graphics, const EnvironmentTexture& environment, const Buffer& environmentBuffer, float skyAerosols = 1.0F)
	{
		std::shared_ptr<void> used;
		RecordOnGraphics(graphics, [&](CommandBufferHandle cmd) { used = RecordEnvironment(cmd, graphics, environment, environmentBuffer, skyAerosols); });
		RetireAfterGraphicsWork(graphics, std::move(used));
	}

	// uploads gLights if it changed: the model's lights, then the environment's dominant lights as directional lights
	// (turned and scaled as the environment is: they were taken out of it, see environment::ExtractDominantLights), or
	// before any environment is installed, the sky's sun where the time of day puts it. if one of the model's directional
	// lights already shines from where one of them does (within kMergeAngle), they are the same light, merged: the model's
	// (as authored, and maybe animated) is kept alone, unless merging is off (mySettings.mergeLights). call on the draw thread.
	void SyncLights(RHI& rhi, QueueTimelineContextData& graphics)
	{
		std::vector<LightData> lights = mySceneLightData;
		// the model's lights' sizes: the ui's, else a directional light the sun's, a point or spot light none
		{
			auto sizes = myLightSizes.Read();
			const auto& set = sizes.Get();
			for (size_t lightIt = 0; lightIt < lights.size(); lightIt++)
			{
				auto& light = lights[lightIt];
				float size = light.type == LIGHT_TYPE_DIRECTIONAL ? environment::kSkySunRadius : 0.0F;
				if (lightIt < set.size() && !std::isnan(set[lightIt]))
					size = std::max(light.type == LIGHT_TYPE_DIRECTIONAL ? glm::radians(set[lightIt]) : set[lightIt], 0.0F);
				light.size = size;
			}
		}
		std::vector<environment::DominantLight> dominants = myEnvironmentLights ? *myEnvironmentLights : SkyLights(SkyNow());

		// the panorama's space to the world's (the inverse of the shader's EnvironmentDirection)
		float angle = glm::radians(mySettings.environmentRotationDegrees.load(std::memory_order_relaxed));
		float intensity = mySettings.environmentIntensity.load(std::memory_order_relaxed);
		float c = std::cos(angle);
		float s = std::sin(angle);
		float cosMerge = std::cos(glm::radians(kMergeAngle));
		bool merge = mySettings.mergeLights.load(std::memory_order_relaxed);

		std::vector<DominantLightInfo> info;
		for (const auto& dominant : dominants)
		{
			const auto& d = dominant.direction;
			LightData light{};
			light.direction[0] = -((c * d[0]) + (s * d[2]));
			light.direction[1] = -d[1];
			light.direction[2] = -((-s * d[0]) + (c * d[2]));
			for (size_t channel = 0; channel < 3; channel++)
				light.intensity[channel] = dominant.irradiance[channel] * intensity;
			light.type = LIGHT_TYPE_DIRECTIONAL;
			light.size = dominant.angularRadius;

			glm::vec3 direction(light.direction[0], light.direction[1], light.direction[2]);
			bool merged = merge && std::ranges::any_of(
									   mySceneLightData,
									   [&direction, cosMerge](const LightData& sceneLight)
									   {
										   glm::vec3 sceneDirection(sceneLight.direction[0], sceneLight.direction[1], sceneLight.direction[2]);
										   return sceneLight.type == LIGHT_TYPE_DIRECTIONAL && glm::length(sceneDirection) > 0.0F &&
												  glm::dot(glm::normalize(sceneDirection), glm::normalize(direction)) >= cosMerge;
									   });
			// a model's lights leave room for the dominant lights (see UpdateLights)
			if (!merged)
				lights.push_back(light);
			info.push_back(
				{.lux = (0.2126F * dominant.irradiance[0]) + (0.7152F * dominant.irradiance[1]) + (0.0722F * dominant.irradiance[2]),
				 .merged = merged});
		}
		// for the ui, when it changed (one lock at a time)
		bool changed = false;
		{
			auto shown = myEnvironmentLightInfo.Read();
			changed = shown.Get() != info;
		}
		if (changed)
			myEnvironmentLightInfo.Write().Get() = std::move(info);

		auto bytes = std::as_bytes(std::span(lights));
		if (lights.size() == myLightData.size() && std::ranges::equal(bytes, std::as_bytes(std::span(myLightData))))
			return;
		UpdateBufferOnGraphics(graphics, *rhi.GetPrimaryDevice().GetResource<Buffer>(myLightsUuid), 0, bytes);
		myLightCount = static_cast<uint32_t>(lights.size());
		myLightData = std::move(lights);
	}


	// writes texture views, starting at slot first (see UpdateBufferOnGraphics). call on the draw thread.
	void UpdateTextureViews(
		RHI& rhi, QueueTimelineContextData& graphics, uint32_t first, std::span<const TextureView> views)
	{
		ENSURE(first + views.size() <= SHADER_TYPES_TEXTURE_VIEW_COUNT);

		UpdateBufferOnGraphics(
			graphics, *rhi.GetPrimaryDevice().GetResource<Buffer>(myTextureViewsUuid), first * sizeof(TextureView), std::as_bytes(views));
	}


	// a frame's histograms (gExposureHistogram, read once the frame's previous use is done). auto exposure: what the
	// scene's luminance is, the mean log2 luminance of the pixels between the 50th and 95th percentile (the darkest and
	// the brightest left out; the lit pixels weighing most at night, where enough of the view is lit: see kLitShare), toward
	// which the exposure moves, a little each frame (by elapsed time), so that it is kAutoExposureKey, less up to
	// kNightStops as the scene darkens from daylight's luminance to a lit street's at night (see kDaylightLog2): a night
	// shown darker than a day, as the eye and a photographer have it (with one key, Bistro at night was lifted 8.7 stops
	// to look as bright as at noon, 2.5). and how far the camera looks: the median of the drawn
	// pixels' distances from the eye, toward which myLookDistances moves the same way (the indirect lighting's finer clips
	// are centered toward it, see gi::MakeClipmap). each view by its own pixels (split screen's: a sunlit roof beside a dark
	// corridor, one exposure for both blew the roof out). call on the draw thread.
	void UpdateAutoExposure(RHI& rhi, uint32_t frameIndex)
	{
		constexpr double kAdaptationSpeed = 2.0; // per second, of what is left to the target, as an exponential decay

		auto now = std::chrono::steady_clock::now();
		auto elapsed = myAutoExposureLast ? std::chrono::duration<double>(now - *myAutoExposureLast).count() : 0.0;
		myAutoExposureLast = now;
		// a view's first measure is taken as it is (a view split screen adds starts settled). by the time elapsed, whatever
		// it is, so that the exposure takes as long to settle at any frame rate (a long frame, a load's, used to take the
		// target at once past a second: whether it did depended on how the frames went)
		auto adapt = [elapsed](std::atomic<float>& value, bool& measured, float target)
		{
			auto current = value.load(std::memory_order_relaxed);
			if (elapsed <= 0.0 || !measured)
				current = target;
			else
				current += (target - current) * static_cast<float>(1.0 - std::exp(-elapsed * kAdaptationSpeed));
			measured = true;
			value.store(current, std::memory_order_relaxed);
		};

		auto& histogram = *rhi.GetPrimaryDevice().GetResource<Buffer>(myExposureHistogramUuid);
		auto memory = histogram.Map();
		std::array<uint32_t, SHADER_TYPES_FRAME_HISTOGRAM_BINS> views{};
		std::memcpy(views.data(), memory.data() + (static_cast<size_t>(frameIndex) * sizeof(views)), sizeof(views));
		histogram.Unmap();

		auto grid = myViews.GetGrid();
		for (size_t viewIt = 0; viewIt < std::min<size_t>(static_cast<size_t>(grid.x) * grid.y, SHADER_TYPES_SPLIT_VIEW_COUNT); viewIt++)
			UpdateViewExposure(viewIt, std::span(views).subspan(viewIt * SHADER_TYPES_HISTOGRAM_BINS, SHADER_TYPES_HISTOGRAM_BINS), adapt);
	}

	// a view's auto exposure and look distance, by its histograms (see UpdateAutoExposure)
	template <typename Adapt>
	void UpdateViewExposure(size_t view, std::span<const uint32_t> bins, Adapt&& adapt)
	{
		constexpr double kAutoExposureKey = 0.2;
		// the scene's mean log2 luminance (in the renderer's units) of daylight and of night: 2^-4, about 2900 cd/m^2 scaled
		// as the sky's sun is (see SHADER_TYPES_ATMOSPHERE_SUN_ILLUMINANCE), EV100 14, a sunny street (Bistro's at noon, -4.2),
		// and 2^-11, 22 cd/m^2, EV100 7, a lit street at night (Bistro's, -10.5); between them the key falls linearly
		constexpr double kDaylightLog2 = -4.0;
		constexpr double kNightLog2 = -11.0;
		constexpr double kNightStops = 2.0;
		// and on down to moonlight, 2^-21 (a full moon's ground, 0.01 cd/m^2), by kMoonlightStops more: a moonlit night
		// darker still, as the eye has it (the user's call, 2026-11-05), seen by its rods (ViewData::scotopic, see
		// Scotopic): the moonlit ground about 0.02 linear, a dark gray the display still shows (3 stops more, 0.006, was
		// black but for the highlights)
		constexpr double kMoonlightLog2 = -21.0;
		constexpr double kMoonlightStops = 1.3;
		// the eye's night vision takes over as the scene darkens through the mesopic range, from 3 cd/m^2 (2^-14) to 0.003
		// (2^-24): the rods alone under a quarter moon
		constexpr double kPhotopicLog2 = -14.0;
		constexpr double kScotopicLog2 = -24.0;
		{
			std::span<const uint32_t> distances(bins.data() + SHADER_TYPES_EXPOSURE_BINS, SHADER_TYPES_DISTANCE_BINS);
			uint64_t drawn = std::accumulate(distances.begin(), distances.end(), uint64_t{0});
			if (drawn > 0)
			{
				uint64_t counted = 0;
				size_t medianBin = 0;
				for (; medianBin + 1 < distances.size(); medianBin++)
				{
					counted += distances[medianBin];
					if (counted * 2 >= drawn)
						break;
				}
				auto log2Distance = SHADER_TYPES_DISTANCE_LOG2_MIN +
									((static_cast<double>(medianBin) + 0.5) / SHADER_TYPES_DISTANCE_BINS * (SHADER_TYPES_DISTANCE_LOG2_MAX - SHADER_TYPES_DISTANCE_LOG2_MIN));
				adapt(myLookDistances[view], myLookDistanceMeasured[view], static_cast<float>(std::exp2(log2Distance)));
			}
			// and the autofocus modes' (see MeasuredFocusUnits): the median of what is under the view's center spot and
			// its center zone, and the nearest tenth of the whole view; where nothing is drawn there (the sky), each stays
			auto percentile = [&adapt](std::span<const uint32_t> counts, uint64_t numerator, uint64_t denominator, std::atomic<float>& distance, bool& measured)
			{
				uint64_t total = std::accumulate(counts.begin(), counts.end(), uint64_t{0});
				if (total == 0)
					return;
				uint64_t counted = 0;
				size_t bin = 0;
				for (; bin + 1 < counts.size(); bin++)
				{
					counted += counts[bin];
					if (counted * denominator >= total * numerator)
						break;
				}
				auto log2Distance = SHADER_TYPES_DISTANCE_LOG2_MIN +
									((static_cast<double>(bin) + 0.5) / static_cast<double>(counts.size()) * (SHADER_TYPES_DISTANCE_LOG2_MAX - SHADER_TYPES_DISTANCE_LOG2_MIN));
				adapt(distance, measured, static_cast<float>(std::exp2(log2Distance)));
			};
			const auto* focusBins = bins.data() + SHADER_TYPES_EXPOSURE_BINS + SHADER_TYPES_DISTANCE_BINS;
			percentile(std::span(focusBins, SHADER_TYPES_FOCUS_BINS), 1, 2, myFocusDistances[view], myFocusMeasured[view]);
			percentile(std::span(focusBins + SHADER_TYPES_FOCUS_BINS, SHADER_TYPES_FOCUS_BINS), 1, 2, myZoneDistances[view], myZoneMeasured[view]);
			percentile(distances, 1, 10, myNearDistances[view], myNearMeasured[view]);
		}

		// at night the lit parts of the view govern: each pixel weighs whole in the percentiles where it is in the eye's
		// photopic range (above kPhotopicLog2: every daylit surface, a lit street at 2^-11), and below it by its
		// luminance's share of that level (the mesopic range: a dim street between its lamps, a twilight sky, a moonlit
		// ground), as the eye's adaptation and the viewer's attention favour what is lit. by the whole view's percentiles
		// a lit street under a night sky, from above, with the sky and the unlit roofs over half of the view (the user's
		// pose 543.0920,1718.0579,716.2755,-20.047,-76.637 over Bistro at 21:52 on the 21st of June: p50 -25, p95 -11),
		// read as moonlight (a mean of -19), and its facades, lit at 2^-11, came out 8 times white. the weighed
		// percentiles' mean stands where the weight is kLitShare of the view or more, blended in stops from the plain
		// percentiles' below it (a lit street panned off the view's bottom hands the exposure to the sky over the last
		// kLitShare of it; a lamp in a dark field is a highlight), and a view with nothing lit, moonlight alone, keys on
		// its own percentiles, as before: the exposure follows the moon down. the day is photopic throughout (a daylit
		// interior is 10 to 100 cd/m^2; Sponza's darkest twentieth by day, 2^-14.5, weighs 0.7): unchanged. a step at a
		// level (whole above 2^-16, none below 2^-20) counted a dusk sky whole until it fell out: the exposure rose 1.3
		// stops as the sky dimmed and fell back 2 once it was gone, a dip in a time lapse; by the luminance the sky's
		// weight fades with its light, and the dip is half a stop
		constexpr double kLitShare = 0.03;

		uint64_t total = std::accumulate(bins.begin(), bins.begin() + SHADER_TYPES_EXPOSURE_BINS, uint64_t{0});
		if (total == 0)
			return;
		auto binLog2Luminance = [](size_t binIt)
		{
			return SHADER_TYPES_EXPOSURE_LOG2_MIN +
				   ((static_cast<double>(binIt) + 0.5) / SHADER_TYPES_EXPOSURE_BINS * (SHADER_TYPES_EXPOSURE_LOG2_MAX - SHADER_TYPES_EXPOSURE_LOG2_MIN));
		};
		auto litWeight = [&binLog2Luminance](size_t binIt) { return std::exp2(std::min(binLog2Luminance(binIt) - kPhotopicLog2, 0.0)); };
		// the mean log2 luminance of the pixels between the 50th and 95th percentile of the histogram with each bin's
		// pixels weighed by weightOf, none where it weighs nothing
		auto percentileMean = [&bins, &binLog2Luminance](auto&& weightOf) -> std::optional<double>
		{
			double weighted = 0.0;
			for (size_t binIt = 0; binIt < SHADER_TYPES_EXPOSURE_BINS; binIt++)
				weighted += weightOf(binIt) * bins[binIt];
			if (weighted <= 0.0)
				return std::nullopt;
			auto low = weighted * 0.5;
			auto high = weighted * 0.95;
			double counted = 0.0;
			double sum = 0.0;
			double weight = 0.0;
			for (size_t binIt = 0; binIt < SHADER_TYPES_EXPOSURE_BINS; binIt++)
			{
				// the part of the bin between the percentiles
				auto count = weightOf(binIt) * bins[binIt];
				auto from = std::max(counted, low);
				auto to = std::min(counted + count, high);
				counted += count;
				if (to <= from)
					continue;
				sum += binLog2Luminance(binIt) * (to - from);
				weight += to - from;
			}
			if (weight <= 0.0)
				return std::nullopt;
			return sum / weight;
		};
		auto meanAll = percentileMean([](size_t) { return 1.0; });
		if (!meanAll)
			return;
		double mean = *meanAll;
		double litShare = 0.0;
		for (size_t binIt = 0; binIt < SHADER_TYPES_EXPOSURE_BINS; binIt++)
			litShare += litWeight(binIt) * bins[binIt];
		litShare /= static_cast<double>(total);
		double lit = std::clamp(litShare / kLitShare, 0.0, 1.0);
		if (lit > 0.0)
			if (auto meanLit = percentileMean(litWeight))
				mean = std::lerp(*meanAll, *meanLit, lit);
		double darkness = std::clamp((kDaylightLog2 - mean) / (kDaylightLog2 - kNightLog2), 0.0, 1.0);
		// the night vision's share by the scene's luminance, whether or not the exposure follows it
		double night = std::clamp((kNightLog2 - mean) / (kNightLog2 - kMoonlightLog2), 0.0, 1.0);
		double scotopic = std::clamp((kPhotopicLog2 - mean) / (kPhotopicLog2 - kScotopicLog2), 0.0, 1.0);
		adapt(myScotopic[view], myScotopicMeasured[view], static_cast<float>(scotopic));
		double target = std::log2(kAutoExposureKey) - (kNightStops * darkness) - (kMoonlightStops * night) - mean;
		// SPEEDO_EXPOSURE_LOG=1: what the exposure keys on, every 30th frame: the histogram's percentiles, the lit share,
		// the mean and the target
		static const bool kLog = [] { const char* log = std::getenv("SPEEDO_EXPOSURE_LOG"); return log != nullptr && std::string_view(log) == "1"; }();
		if (kLog && view == 0 && (myExposureLogFrame++ % 30) == 0)
		{
			auto percentile = [&bins, &binLog2Luminance, total](double share)
			{
				double at = static_cast<double>(total) * share;
				double count = 0.0;
				for (size_t binIt = 0; binIt < SHADER_TYPES_EXPOSURE_BINS; binIt++)
				{
					count += bins[binIt];
					if (count >= at)
						return binLog2Luminance(binIt);
				}
				return static_cast<double>(SHADER_TYPES_EXPOSURE_LOG2_MAX);
			};
			std::println(
				"exposure: p5 {:.1f} p25 {:.1f} p50 {:.1f} p75 {:.1f} p95 {:.1f} p99 {:.1f} lit {:.3f} mean {:.2f} (all {:.2f}) darkness {:.2f} night {:.2f} scotopic {:.2f} target {:+.2f} current {:+.2f}",
				percentile(0.05), percentile(0.25), percentile(0.5), percentile(0.75), percentile(0.95), percentile(0.99), litShare, mean, *meanAll, darkness, night, scotopic,
				target, myAutoExposureStops[view].load(std::memory_order_relaxed));
		}
		if (!mySettings.AutoExposure())
			return;
		adapt(myAutoExposureStops[view], myAutoExposureMeasured[view], static_cast<float>(target));
	}


	// what KHR_animation_pointer channels (and lights that follow nodes) set, as the model's last Animate left it: patches
	// and uploads the materials, texture views and lights that changed. call on the draw thread.
	void ApplyPointerValues(RHI& rhi, QueueTimelineContextData& graphics, const Model& model)
	{
		const auto& animation = model.GetDesc().animation;
		if (animation.pointerTargets.empty() && animation.lightLinks.empty())
			return;
		auto values = model.GetPointerValues();
		auto valuesOf = [&values](const ScenePointerTarget& target)
		{
			return target.valueBase + target.valueCount <= values.size() ? values.subspan(target.valueBase, target.valueCount)
																		 : std::span<const float>{};
		};

		// the materials, uploaded as one range from the first that changed to the last
		std::optional<size_t> first;
		size_t last = 0;
		for (size_t targetIt = 0; targetIt < animation.pointerTargets.size(); targetIt++)
		{
			const auto& target = animation.pointerTargets[targetIt];
			if (target.kind != ScenePointerTarget::Kind::kMaterial || target.index >= myModelMaterialData.size() ||
				target.index >= model.GetDesc().materials.size())
				continue;
			// the emissive factor times its strength, the target after it (see MaterialProperty::kEmissive)
			auto property = MaterialProperty{target.property};
			if (property == MaterialProperty::kEmissiveStrength)
				continue;
			float scale = 1.0F;
			if (property == MaterialProperty::kEmissive && targetIt + 1 < animation.pointerTargets.size())
				if (auto strength = valuesOf(animation.pointerTargets[targetIt + 1]); !strength.empty())
					scale = strength[0];
			auto& data = myModelMaterialData[target.index];
			auto before = data;
			ApplyMaterialProperty(data, model.GetDesc().materials[target.index], property, valuesOf(target), scale);
			if (std::memcmp(&before, &data, sizeof(MaterialData)) != 0)
			{
				first = std::min<size_t>(first.value_or(target.index), target.index);
				last = std::max<size_t>(last, target.index);
			}
		}
		if (first)
			UpdateMaterials(rhi, graphics, static_cast<uint32_t>(1 + *first), std::span(myModelMaterialData).subspan(*first, last - *first + 1));

		// the texture transforms (offset, rotation and scale, the three targets from the offset's)
		for (auto& animated : myAnimatedTextureViews)
		{
			if (animated.target + 2 >= animation.pointerTargets.size())
				continue;
			auto offset = valuesOf(animation.pointerTargets[animated.target]);
			auto rotation = valuesOf(animation.pointerTargets[animated.target + 1]);
			auto scale = valuesOf(animation.pointerTargets[animated.target + 2]);
			if (offset.size() < 2 || rotation.empty() || scale.size() < 2)
				continue;
			auto transform = TextureTransform({offset[0], offset[1]}, rotation[0], {scale[0], scale[1]});
			auto view = animated.data;
			std::ranges::copy(std::span(transform).first(3), view.uTransform);
			std::ranges::copy(std::span(transform).last(3), view.vTransform);
			if (std::memcmp(&view, &animated.data, sizeof(TextureView)) != 0)
			{
				animated.data = view;
				UpdateTextureViews(rhi, graphics, animated.view, std::span(&view, 1));
			}
		}

		// the lights that follow nodes (and their visibility), and whose values animate
		if (model.AnimatesLights())
		{
			auto lights = model.GetLights();
			auto same = [](const SceneLight& a, const SceneLight& b)
			{
				return a.position == b.position && a.direction == b.direction && a.intensity == b.intensity && a.color == b.color &&
					   a.range == b.range && a.innerConeAngle == b.innerConeAngle && a.outerConeAngle == b.outerConeAngle;
			};
			if (!std::ranges::equal(lights, myAnimatedLights, same))
			{
				myAnimatedLights.assign(lights.begin(), lights.end());
				UpdateLights(lights);
			}
		}

		// the views, when they look through a camera that animates
		if (model.AnimatesCameras())
			myViews.UpdateSceneCameras(model.GetCameras());
	}


	// binds what the device's ray queries trace (gBvhScene, a top level structure per frame index). the main layout bound
	void BindAccelerationStructures(const AccelerationStructures& structures)
	{
		for (uint32_t frameIt = 0; frameIt < SHADER_TYPES_FRAME_COUNT; frameIt++)
			myPipeline.SetDescriptorData(
				"gBvhScene",
				static_cast<::AccelerationStructureHandle<rhi::kGraphicsApi>>(structures.GetTopLevel(frameIt)),
				DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
				frameIt);
	}

	// makes an uploaded model the one being drawn, with its materials and their textures (by material), retiring the
	// previous ones. call on the draw thread.
	void InstallModel(
		RHI& rhi,
		QueueTimelineContextData& graphics,
		const std::shared_ptr<Model>& model,
		std::vector<MaterialTextures> textures,
		std::vector<std::string> filePaths,
		ModelArrangement arrangement)
	{
		ZoneScopedN("WindowedApplication::InstallModel");

		rhi::CrashEvent(std::format("model installed: {}", model->GetDesc().name));

		// each resource once: materials share textures
		Uploads uploads;
		for (const auto* buffer : model->GetUploadedBuffers())
			uploads.buffers.emplace_back(buffer, model->GetUpload());
		for (const auto& material : textures)
			for (const auto* texture :
				 {&material.diffuse, &material.alpha, &material.normal, &material.emissive, &material.occlusion, &material.metallicRoughness,
				  &material.specular, &material.specularColor, &material.clearcoat, &material.clearcoatRoughness, &material.clearcoatNormal,
				  &material.sheenColor, &material.sheenRoughness, &material.transmission, &material.thickness,
				  &material.anisotropy, &material.iridescence, &material.iridescenceThickness,
				  &material.diffuseTransmission, &material.diffuseTransmissionColor, &material.retroreflection, &material.multiscatterColor})
				if (texture->image &&
					std::ranges::none_of(uploads.images, [&texture](const auto& image) { return image.first == texture->image; }))
					uploads.images.emplace_back(texture->image, texture->upload);

		// everything is switched at once, after the textures are readable: one descriptor set update, and no frame draws
		// the new model with the old materials or the other way around
		TransitionThenBind(rhi, graphics, uploads, [this, &rhi, model, textures = std::move(textures), filePaths = std::move(filePaths), arrangement](QueueTimelineContextData& graphics) mutable
		{
			auto& device = rhi.GetPrimaryDevice();
			auto& pipeline = myPipeline;
			const auto& blackView = *device.GetResource<ImageView>(myBlackTextureViewUuid);

			pipeline.BindLayoutAuto(pipeline.GetLayoutHandle("Main"), PipelineBindPoint::kGraphics);

			// each texture once, in the slots from kModelTextureFirstSlot. the previous model's slots go back to black.
			std::vector<std::pair<uuids::uuid, uuids::uuid>> textureUuids;
			core::UnorderedMap<const Image*, uint32_t> textureSlots;
			auto slotOf = [&](const Texture& texture) -> std::optional<uint32_t>
			{
				const auto& [image, view, upload, normalYUp] = texture;
				if (!image)
					return std::nullopt;

				if (auto slotIt = textureSlots.find(image.get()); slotIt != textureSlots.end())
					return slotIt->second;

				ENSUREF(textureUuids.size() < kModelTextureMaxCount, "more than {} textures in a model", kModelTextureMaxCount);

				auto slot = static_cast<uint32_t>(kModelTextureFirstSlot + textureUuids.size());
				pipeline.SetDescriptorData(
					"gTextures",
					ImageBinding{.sampler = {}, .imageView = *view, .layout = image->GetDesc().layout},
					DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
					slot);

				device.AddResource(image);
				device.AddResource(view);
				textureUuids.emplace_back(image->GetUuid(), view->GetUuid());
				textureSlots.emplace(image.get(), slot);

				return slot;
			};

			const auto& descMaterials = model->GetDesc().materials;

			// each distinct sampler once: the default in its slot, the others in the rest (see TextureRef::sampler)
			std::vector<SamplerDesc> samplerDescs;
			auto samplerSlotOf = [&samplerDescs](const SamplerDesc& desc) -> uint32_t
			{
				static const SamplerDesc kDefault = TextureRef{}.sampler;
				if (desc == kDefault)
					return kDefaultSamplerId;
				auto samplerDescIt = std::ranges::find(samplerDescs, desc);
				if (samplerDescIt == samplerDescs.end())
				{
					ENSUREF(
						samplerDescs.size() < kModelSamplerSlots.size(),
						"more than {} different samplers in a model (kModelSamplerSlots)",
						kModelSamplerSlots.size());
					samplerDescIt = samplerDescs.insert(samplerDescs.end(), desc);
				}
				return kModelSamplerSlots[static_cast<size_t>(samplerDescIt - samplerDescs.begin())];
			};

			// each distinct view once, from 1 (0 is material 0's, see InstallImage)
			std::vector<TextureView> views;
			std::vector<AnimatedTextureView> animatedViews;
			core::UnorderedMap<TextureViewKey, uint32_t, TextureViewKeyHash> viewIds;
			auto viewOf = [&, this](const Texture& texture, const TextureRef& ref) -> std::optional<uint32_t>
			{
				auto textureSlot = slotOf(texture);
				if (!textureSlot)
					return std::nullopt;

				auto samplerSlot = samplerSlotOf(ref.sampler);
				auto [viewIdIt, inserted] = viewIds.try_emplace(
					TextureViewKey{
						.textureSlot = *textureSlot,
						.samplerSlot = samplerSlot,
						.texCoord = ref.texCoord,
						.transform = std::bit_cast<std::array<uint32_t, 6>>(ref.transform),
						.animated = static_cast<uint32_t>(ref.animatedTransform + 1),
						.flags = texture.normalYUp ? TEXTURE_VIEW_FLAG_NORMAL_Y_UP : 0U},
					0U);
				if (inserted)
				{
					ENSUREF(
						1 + views.size() < SHADER_TYPES_TEXTURE_VIEW_COUNT,
						"more than {} texture views in a model (SHADER_TYPES_TEXTURE_VIEW_COUNT)",
						SHADER_TYPES_TEXTURE_VIEW_COUNT - 1);
					viewIdIt->second = static_cast<uint32_t>(1 + views.size());
					views.push_back(MakeTextureView(*textureSlot, samplerSlot, ref, texture.normalYUp ? TEXTURE_VIEW_FLAG_NORMAL_Y_UP : 0U));
					if (ref.animatedTransform >= 0)
						animatedViews.push_back({.view = viewIdIt->second, .target = static_cast<uint32_t>(ref.animatedTransform), .data = views.back()});
				}
				return viewIdIt->second;
			};

			std::vector<MaterialData> materials(textures.size(), DefaultMaterialData());
			for (size_t materialIt = 0; materialIt < materials.size(); materialIt++)
			{
				auto& material = materials[materialIt];
				const auto& desc = descMaterials[materialIt];
				std::ranges::fill(material.color, 1.0F);
				material.alphaCutoff = desc.alphaCutoff;
				if (model->GetGroundMaterial() == materialIt || model->GetHorizonMaterial() == materialIt)
					material.checker = kGroundChecker;
				std::copy_n(glm::value_ptr(desc.emissive), 3, material.emissive);
				material.metallic = desc.metallic;
				material.roughness = desc.roughness;
				material.specular = desc.specular;
				std::copy_n(glm::value_ptr(desc.specularColor), 3, material.specularColor);
				material.specularColor[3] = desc.ior;
				if (desc.specularGlossiness)
				{
					material.flags |= MATERIAL_FLAG_SPECULAR_GLOSSINESS;
					material.roughness = desc.glossiness;
				}
				if (auto view = viewOf(textures[materialIt].specular, desc.specularTexture))
				{
					material.specularView = *view;
					material.flags |= MATERIAL_FLAG_SPECULAR_TEXTURE;
				}
				if (auto view = viewOf(textures[materialIt].specularColor, desc.specularColorTexture))
				{
					material.specularColorView = *view;
					material.flags |= MATERIAL_FLAG_SPECULAR_COLOR_TEXTURE;
				}
				auto layers = LayersOf(model->GetDesc(), materialIt);
				if (layers.clearcoat)
				{
					material.flags |= MATERIAL_FLAG_CLEARCOAT;
					material.clearcoat[0] = desc.clearcoat;
					material.clearcoat[1] = desc.clearcoatRoughness;
					material.clearcoat[2] = desc.clearcoatNormalScale;
					for (auto [texture, ref, view, flag] : std::array{
							 std::tuple{&textures[materialIt].clearcoat, &desc.clearcoatTexture, &material.clearcoatView, MATERIAL_FLAG_CLEARCOAT_TEXTURE},
							 std::tuple{&textures[materialIt].clearcoatRoughness, &desc.clearcoatRoughnessTexture, &material.clearcoatRoughnessView, MATERIAL_FLAG_CLEARCOAT_ROUGHNESS_TEXTURE},
							 std::tuple{&textures[materialIt].clearcoatNormal, &desc.clearcoatNormalTexture, &material.clearcoatNormalView, MATERIAL_FLAG_CLEARCOAT_NORMAL_TEXTURE}})
						if (auto id = viewOf(*texture, *ref))
						{
							*view = *id;
							material.flags |= flag;
						}
				}
				if (layers.sheen)
				{
					material.flags |= MATERIAL_FLAG_SHEEN;
					std::copy_n(glm::value_ptr(desc.sheenColor), 3, material.sheen);
					material.sheen[3] = desc.sheenRoughness;
					for (auto [texture, ref, view, flag] : std::array{
							 std::tuple{&textures[materialIt].sheenColor, &desc.sheenColorTexture, &material.sheenColorView, MATERIAL_FLAG_SHEEN_COLOR_TEXTURE},
							 std::tuple{&textures[materialIt].sheenRoughness, &desc.sheenRoughnessTexture, &material.sheenRoughnessView, MATERIAL_FLAG_SHEEN_ROUGHNESS_TEXTURE}})
						if (auto id = viewOf(*texture, *ref))
						{
							*view = *id;
							material.flags |= flag;
						}
				}
				if (layers.transmission)
				{
					material.flags |= MATERIAL_FLAG_TRANSMISSION;
					material.transmission[0] = desc.transmission;
					material.transmission[3] = desc.dispersion;
					if (auto id = viewOf(textures[materialIt].transmission, desc.transmissionTexture))
					{
						material.transmissionView = *id;
						material.flags |= MATERIAL_FLAG_TRANSMISSION_TEXTURE;
					}
				}
				// the volume: what transmission refracts through, and what diffuse transmission and scattering light through
				if (layers.transmission || layers.diffuseTransmission || layers.volumeScatter)
				{
					material.transmission[1] = desc.thickness;
					material.transmission[2] = desc.attenuationDistance;
					std::copy_n(glm::value_ptr(desc.attenuationColor), 3, material.attenuationColor);
					if (auto id = viewOf(textures[materialIt].thickness, desc.thicknessTexture))
					{
						material.thicknessView = *id;
						material.flags |= MATERIAL_FLAG_THICKNESS_TEXTURE;
					}
				}
				if (layers.anisotropy)
				{
					material.flags |= MATERIAL_FLAG_ANISOTROPY;
					material.anisotropy[0] = desc.anisotropy;
					material.anisotropy[1] = std::cos(desc.anisotropyRotation);
					material.anisotropy[2] = std::sin(desc.anisotropyRotation);
					if (auto id = viewOf(textures[materialIt].anisotropy, desc.anisotropyTexture))
					{
						material.anisotropyView = *id;
						material.flags |= MATERIAL_FLAG_ANISOTROPY_TEXTURE;
					}
				}
				if (layers.iridescence)
				{
					material.flags |= MATERIAL_FLAG_IRIDESCENCE;
					material.iridescence[0] = desc.iridescence;
					material.iridescence[1] = desc.iridescenceIor;
					material.iridescence[2] = desc.iridescenceThicknessMin;
					material.iridescence[3] = desc.iridescenceThicknessMax;
					for (auto [texture, ref, view, flag] : std::array{
							 std::tuple{&textures[materialIt].iridescence, &desc.iridescenceTexture, &material.iridescenceView, MATERIAL_FLAG_IRIDESCENCE_TEXTURE},
							 std::tuple{&textures[materialIt].iridescenceThickness, &desc.iridescenceThicknessTexture, &material.iridescenceThicknessView, MATERIAL_FLAG_IRIDESCENCE_THICKNESS_TEXTURE}})
						if (auto id = viewOf(*texture, *ref))
						{
							*view = *id;
							material.flags |= flag;
						}
				}
				if (layers.diffuseTransmission)
				{
					material.flags |= MATERIAL_FLAG_DIFFUSE_TRANSMISSION;
					std::copy_n(glm::value_ptr(desc.diffuseTransmissionColor), 3, material.diffuseTransmission);
					material.diffuseTransmission[3] = desc.diffuseTransmission;
					for (auto [texture, ref, view, flag] : std::array{
							 std::tuple{&textures[materialIt].diffuseTransmission, &desc.diffuseTransmissionTexture, &material.diffuseTransmissionView, MATERIAL_FLAG_DIFFUSE_TRANSMISSION_TEXTURE},
							 std::tuple{&textures[materialIt].diffuseTransmissionColor, &desc.diffuseTransmissionColorTexture, &material.diffuseTransmissionColorView, MATERIAL_FLAG_DIFFUSE_TRANSMISSION_COLOR_TEXTURE}})
						if (auto id = viewOf(*texture, *ref))
						{
							*view = *id;
							material.flags |= flag;
						}
				}
				if (layers.volumeScatter)
				{
					// the scatter pass writes its material + 1 to a half float (see SHADER_TYPES_SCATTER_TEXTURE), exact to 2048
					ENSUREF(
						materialIt + 2 <= 2048,
						"scattering material {} is past the 2047 the scatter texture tells apart",
						materialIt + 1);
					material.flags |= MATERIAL_FLAG_VOLUME_SCATTER;
					std::copy_n(glm::value_ptr(desc.multiscatterColor), 3, material.multiscatterColor);
					if (auto id = viewOf(textures[materialIt].multiscatterColor, desc.multiscatterColorTexture))
					{
						material.multiscatterColorView = *id;
						material.flags |= MATERIAL_FLAG_VOLUME_SCATTER_TEXTURE;
					}
				}
				if (layers.retroreflection)
				{
					material.flags |= MATERIAL_FLAG_RETROREFLECTION;
					material.retroreflection = desc.retroreflection;
					if (auto id = viewOf(textures[materialIt].retroreflection, desc.retroreflectionTexture))
					{
						material.retroreflectionView = *id;
						material.flags |= MATERIAL_FLAG_RETROREFLECTION_TEXTURE;
					}
				}
				if (desc.unlit)
					material.flags |= MATERIAL_FLAG_UNLIT;
				if (auto view = viewOf(textures[materialIt].metallicRoughness, desc.metallicRoughnessTexture))
				{
					material.metallicRoughnessView = *view;
					material.flags |= MATERIAL_FLAG_METALLIC_ROUGHNESS_TEXTURE;
				}

				if (auto view = viewOf(textures[materialIt].diffuse, desc.diffuseTexture))
				{
					material.baseColorView = *view;
					material.flags |= MATERIAL_FLAG_TEXTURE;
				}
				if (auto view = viewOf(textures[materialIt].alpha, desc.alphaTexture))
				{
					material.alphaView = *view;
					material.flags |= MATERIAL_FLAG_ALPHA_TEXTURE;
				}
				// the normal map, or a bump texture turned into one (see LoadAndInstallModels)
				if (auto view = viewOf(textures[materialIt].normal, desc.normalTexture.empty() ? desc.bumpTexture : desc.normalTexture))
				{
					material.normalView = *view;
					material.normalScale = desc.normalScale;
					material.flags |= MATERIAL_FLAG_NORMAL_TEXTURE;
				}
				if (auto view = viewOf(textures[materialIt].emissive, desc.emissiveTexture))
				{
					material.emissiveView = *view;
					material.flags |= MATERIAL_FLAG_EMISSIVE_TEXTURE;
				}
				if (auto view = viewOf(textures[materialIt].occlusion, desc.occlusionTexture))
				{
					material.occlusionView = *view;
					material.emissive[3] = desc.occlusionStrength;
					material.flags |= MATERIAL_FLAG_OCCLUSION_TEXTURE;
				}
			}

			// the model's samplers in their slots, and the previous model's other slots back to the default sampler
			auto samplers = std::make_shared<SamplerVector>(
				SamplerVectorCreateDesc{device.CreateDeviceObjectCreateDesc("Model Samplers"), std::vector(samplerDescs)});
			const auto& defaultSampler = (*device.GetResource<SamplerVector>(mySamplersUuid))[0];
			for (size_t slotIt = 0; slotIt < kModelSamplerSlots.size(); slotIt++)
				if (slotIt < samplerDescs.size() || slotIt < myModelSamplerCount)
					pipeline.SetDescriptorData(
						"gSamplers",
						ImageBinding{.sampler = slotIt < samplerDescs.size() ? (*samplers)[slotIt] : defaultSampler},
						DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS,
						kModelSamplerSlots[slotIt]);
			RetireAfterGraphicsWork(graphics, device.ReplaceResource(myModelSamplersUuid, samplers));
			myModelSamplersUuid = samplers->GetUuid();
			myModelSamplerCount = samplerDescs.size();

			UpdateTextureViews(rhi, graphics, 1, views);
			myAnimatedTextureViews = std::move(animatedViews);

			for (size_t slotIt = textureUuids.size(); slotIt < myModelTextureUuids.size(); slotIt++)
				pipeline.SetDescriptorData(
					"gTextures",
					ImageBinding{.sampler = {}, .imageView = blackView, .layout = ImageLayout::kShaderReadOnly},
					DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
					kModelTextureFirstSlot + slotIt);

			for (const auto& [imageUuid, viewUuid] : myModelTextureUuids)
			{
				RetireAfterGraphicsWork(graphics, device.ReplaceResource(viewUuid, nullptr));
				RetireAfterGraphicsWork(graphics, device.ReplaceResource(imageUuid, nullptr));
			}
			myModelTextureUuids = std::move(textureUuids);

			GrowMaterials(rhi, graphics, 1 + materials.size());
			UpdateMaterials(rhi, graphics, 1, materials);
			myModelMaterialData = materials;
			myAnimatedLights.clear();
			myLightSizes.Write().Get().clear();
			UpdateLights(model->GetDesc().lights);
			// and the values animations set, at rest
			ApplyPointerValues(rhi, graphics, *model);

			pipeline.SetDescriptorData(
				"gVertexBuffer",
				BufferBinding{.buffer = model->GetVertexBuffer(), .offset = 0},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
			pipeline.SetDescriptorData(
				"gIndexBuffer", BufferBinding{.buffer = model->GetIndexBuffer(), .offset = 0}, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
			const auto& defaultBvh = *device.GetResource<Buffer>(myDefaultBvhUuid);
			pipeline.SetDescriptorData(
				"gBvhNodes",
				BufferBinding{.buffer = model->GetBvhNodeBuffer() != nullptr ? *model->GetBvhNodeBuffer() : defaultBvh, .offset = 0},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
			pipeline.SetDescriptorData(
				"gBvhTriangles",
				BufferBinding{.buffer = model->GetBvhTriangleBuffer() != nullptr ? *model->GetBvhTriangleBuffer() : defaultBvh, .offset = 0},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
			pipeline.SetDescriptorData(
				"gEmitters",
				BufferBinding{.buffer = model->GetEmitterBuffer() != nullptr ? *model->GetEmitterBuffer() : defaultBvh, .offset = 0},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
			pipeline.SetDescriptorData(
				"gEmitterTree",
				BufferBinding{.buffer = model->GetEmitterTreeBuffer() != nullptr ? *model->GetEmitterTreeBuffer() : defaultBvh, .offset = 0},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
			pipeline.SetDescriptorData(
				"gSkinVertices",
				BufferBinding{
					.buffer = model->GetSkinBuffer() != nullptr ? *model->GetSkinBuffer() : *device.GetResource<Buffer>(myDefaultSkinVerticesUuid),
					.offset = 0,},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
			pipeline.SetDescriptorData(
				"gMorphDeltas",
				BufferBinding{
					.buffer = model->GetMorphDeltaBuffer() != nullptr ? *model->GetMorphDeltaBuffer()
																	  : *device.GetResource<Buffer>(myDefaultMorphDeltasUuid),
					.offset = 0,},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
			for (uint32_t frameIt = 0; frameIt < SHADER_TYPES_FRAME_COUNT; frameIt++)
			{
				pipeline.SetDescriptorData(
					"gModelInstances",
					BufferBinding{.buffer = model->GetInstanceBuffer(frameIt), .offset = 0},
					DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
					frameIt);
				pipeline.SetDescriptorData(
					"gJointMatrices",
					BufferBinding{
						.buffer = model->GetJointBuffer(frameIt) != nullptr ? *model->GetJointBuffer(frameIt)
																			: *device.GetResource<Buffer>(myDefaultJointsUuid),
						.offset = 0,},
					DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
					frameIt);
				pipeline.SetDescriptorData(
					"gMorphWeights",
					BufferBinding{
						.buffer = model->GetMorphWeightBuffer(frameIt) != nullptr ? *model->GetMorphWeightBuffer(frameIt)
																				  : *device.GetResource<Buffer>(myDefaultMorphWeightsUuid),
						.offset = 0,},
					DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
					frameIt);
				for (auto [binding, buffer] : {
						 std::pair{"gBvhMovingNodes", model->GetMovingBvhNodeBuffer(frameIt)},
						 std::pair{"gBvhMovingTriangles", model->GetMovingBvhTriangleBuffer(frameIt)}})
					pipeline.SetDescriptorData(
						binding,
						BufferBinding{.buffer = buffer != nullptr ? *buffer : *device.GetResource<Buffer>(myDefaultBvhUuid), .offset = 0},
						DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
						frameIt);
			}
			// what the device's ray queries trace of it, built in a submission before the frames that trace it (the default's
			// of nothing where it has none)
			if (auto* structures = model->GetAccelerationStructures())
			{
				std::shared_ptr<Buffer> scratch;
				RecordOnGraphics(graphics, [&scratch, structures](CommandBufferHandle cmd) { scratch = structures->RecordBuild(cmd); });
				RetireAfterGraphicsWork(graphics, std::move(scratch));
				BindAccelerationStructures(*structures);
			}
			else if (myDefaultAccelerationStructures)
				BindAccelerationStructures(*myDefaultAccelerationStructures);

			{
				auto scenes = myScenes.Write();
				scenes.Get() = SceneState{
					.filePath = model->GetDesc().name,
					.filePaths = std::move(filePaths),
					.arrangement = arrangement,
					.names = model->GetDesc().scenes,
					.current = model->GetDesc().scene,
					.variants = model->GetDesc().variants,
					.variant = model->GetDesc().variant,
					.ground = model->GetGround(),
					.layout = std::vector(model->GetLayout().begin(), model->GetLayout().end()),
					.bounds = model->GetFramedBounds()};
			}

			// its first animation plays, from the start (SPEEDO_ANIMATION_TIME: paused at that time, e.g. for tests)
			{
				auto animation = myAnimation.Write();
				auto& state = animation.Get();
				state.names.clear();
				for (const auto& clip : model->GetDesc().animation.animations)
					state.names.push_back(clip.name);
				state.selected = state.names.empty() ? std::nullopt : std::optional<size_t>(0);
				state.playing = true;
				state.time = 0.0;
				state.last = std::chrono::steady_clock::now();
				state.previous.reset();
				state.fade = AnimationState::kCrossfade;
				if (const char* time = std::getenv("SPEEDO_ANIMATION_TIME"); time != nullptr && *time != '\0')
				{
					state.playing = false;
					state.time = std::strtod(time, nullptr);
				}
			}

			RetireAfterGraphicsWork(graphics, std::exchange(myModel, model));

			// on a ground plane, framed from above, so that a set's back rows show over its front ones (see model::Merge) and
			// the shadows on the ground are in view, and seen without a far plane: the ground reaches the horizon
			constexpr float kGroundFramePitch = glm::radians(30.0F);
			// and a file's variants laid out, whole (a row of copies fills its bounding sphere: the nearest was cut off)
			myViews.SetScene(
				model->GetDesc().bounds,
				model->GetFramedBounds(),
				model->GetDesc().cameras,
				model->GetGround() == ModelGround::kPlane ? kGroundFramePitch : 0.0F,
				model->GetGround() == ModelGround::kPlane,
				!model->GetLayout().empty());
		});
	}


	// makes an uploaded image the texture sampled by material 0, retiring the previous one. call on the draw thread.
	void InstallImage(
		RHI& rhi,
		QueueTimelineContextData& graphics,
		const std::shared_ptr<Image>& image,
		const std::shared_ptr<ImageView>& imageView,
		const Upload& upload)
	{
		ZoneScopedN("WindowedApplication::InstallImage");

		rhi::CrashEvent(std::format("image installed: {}", image->GetName()));

		TransitionThenBind(rhi, graphics, Uploads{.images = {{image, upload}}}, [this, &rhi, image, imageView](QueueTimelineContextData& graphics)
		{
			auto& device = rhi.GetPrimaryDevice();
			auto& pipeline = myPipeline;

			pipeline.BindLayoutAuto(pipeline.GetLayoutHandle("Main"), PipelineBindPoint::kGraphics);
			pipeline.SetDescriptorData(
				"gTextures",
				ImageBinding{.sampler = {}, .imageView = *imageView, .layout = image->GetDesc().layout},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
				kMaterialTextureId);

			// the default material is untextured until an image is loaded. it samples it through view 0.
			auto view = MakeTextureView(kMaterialTextureId, kDefaultSamplerId, TextureRef{});
			UpdateTextureViews(rhi, graphics, 0, std::span(&view, 1));
			MaterialData material = DefaultMaterialData();
			material.flags = MATERIAL_FLAG_TEXTURE;
			material.alphaCutoff = 0.5F;
			material.baseColorView = 0;
			myDefaultMaterial = material;
			UpdateMaterials(rhi, graphics, 0, std::span(&material, 1));

			RetireAfterGraphicsWork(graphics, device.ReplaceResource(myLoadedImageUuid, image));
			RetireAfterGraphicsWork(graphics, device.ReplaceResource(myLoadedImageViewUuid, imageView));
			myLoadedImageUuid = image->GetUuid();
			myLoadedImageViewUuid = imageView->GetUuid();
		});
	}


	// loads a model (or several, side by side as one, see Model::Load) and its materials' textures, and has the draw thread
	// install them, unless the load was cancelled. call from a load (see gLoads).
	// scene: a gltf file's scene to load (one file only), else its default one. ground: what it stands on
	std::optional<std::string> LoadAndInstallModels(
		RHI& rhi,
		const std::vector<std::string>& filePaths,
		std::atomic_uint8_t& progress,
		const mesh::ImportOptions& options = {},
		ModelArrangement arrangement = ModelArrangement::kSideBySide,
		ModelGround ground = ModelGround::kNone,
		core::CancelledFn cancelled = core::NeverCancelled,
		std::span<const mesh::ImportOptions> fileOptions = {}) // several files' each (see Model::Load of several)
	{
		auto bvh = mySettings.modelBvhs.load(std::memory_order_relaxed) ? ModelBvh::kBuild : ModelBvh::kNone;
		auto model = (options.scene || options.variant) && filePaths.size() == 1
						 ? Model::Load(filePaths.front(), progress, options, bvh, ground, cancelled)
						 : Model::Load(
							   std::vector<std::string_view>(filePaths.begin(), filePaths.end()), progress, arrangement, bvh, ground, cancelled, fileOptions);
		if (!model) // cancelled or failed
			return std::nullopt;

		const auto& filePath = model->GetDesc().name; // or the models' directory, for several

		const auto& materials = model->GetDesc().materials;

		// then the textures, each file and usage once. the progress starts over for them.
		struct TextureLoad
		{
			std::string path;
			std::optional<uint32_t> embeddedImage; // see TextureRef::embeddedImage
			gfx::image::Options options;
			Texture* result;
		};
		std::vector<MaterialTextures> textures(materials.size());
		std::vector<TextureLoad> loads;
		for (size_t materialIt = 0; materialIt < materials.size(); materialIt++)
		{
			const auto& material = materials[materialIt];
			auto& texture = textures[materialIt];
			auto layers = LayersOf(model->GetDesc(), materialIt);
			if (!material.diffuseTexture.empty())
				loads.push_back(
					{material.diffuseTexture.path, material.diffuseTexture.embeddedImage, {.usage = gfx::image::Usage::kColor}, &texture.diffuse});
			if (!material.alphaTexture.empty())
				loads.push_back(
					{material.alphaTexture.path, material.alphaTexture.embeddedImage, {.usage = gfx::image::Usage::kMask}, &texture.alpha});
			if (!material.normalTexture.empty())
				loads.push_back(
					{material.normalTexture.path, material.normalTexture.embeddedImage, {.usage = gfx::image::Usage::kNormal}, &texture.normal});
			else if (!material.bumpTexture.empty())
				loads.push_back(
					{material.bumpTexture.path, material.bumpTexture.embeddedImage, {.usage = gfx::image::Usage::kBump, .bumpScale = material.bumpScale}, &texture.normal});
			// the texture scales emissive, so it is only worth loading if that isn't black (obj map_Ke usually comes with Ke 0)
			if (!material.emissiveTexture.empty() && glm::any(glm::greaterThan(material.emissive, glm::vec3(0.0F))))
				loads.push_back(
					{material.emissiveTexture.path, material.emissiveTexture.embeddedImage, {.usage = gfx::image::Usage::kColor}, &texture.emissive});
			if (!material.occlusionTexture.empty())
				loads.push_back(
					{material.occlusionTexture.path, material.occlusionTexture.embeddedImage, {.usage = gfx::image::Usage::kOcclusion}, &texture.occlusion});
			if (!material.metallicRoughnessTexture.empty())
				loads.push_back(
					{material.metallicRoughnessTexture.path, material.metallicRoughnessTexture.embeddedImage, {.usage = gfx::image::Usage::kMetallicRoughness}, &texture.metallicRoughness});
			if (!material.specularTexture.empty())
				loads.push_back(
					{material.specularTexture.path, material.specularTexture.embeddedImage, {.usage = gfx::image::Usage::kAlpha}, &texture.specular});
			if (!material.specularColorTexture.empty())
				loads.push_back(
					{material.specularColorTexture.path, material.specularColorTexture.embeddedImage, {.usage = gfx::image::Usage::kColor}, &texture.specularColor});
			// the clearcoat's strength is the texture's red, its roughness the green (as kOcclusion and kMetallicRoughness
			// keep them), the sheen roughness the alpha
			if (layers.clearcoat)
				for (auto [ref, usage, result] : std::array{
						 std::tuple{&material.clearcoatTexture, gfx::image::Usage::kOcclusion, &texture.clearcoat},
						 std::tuple{&material.clearcoatRoughnessTexture, gfx::image::Usage::kMetallicRoughness, &texture.clearcoatRoughness},
						 std::tuple{&material.clearcoatNormalTexture, gfx::image::Usage::kNormal, &texture.clearcoatNormal}})
					if (!ref->empty())
						loads.push_back({ref->path, ref->embeddedImage, {.usage = usage}, result});
			// the anisotropy's direction and strength are a linear rgb texture; the iridescence's factor is the red, its
			// thickness the green
			if (layers.anisotropy && !material.anisotropyTexture.empty())
				loads.push_back(
					{material.anisotropyTexture.path, material.anisotropyTexture.embeddedImage, {.usage = gfx::image::Usage::kLinear}, &texture.anisotropy});
			if (layers.iridescence)
				for (auto [ref, usage, result] : std::array{
						 std::tuple{&material.iridescenceTexture, gfx::image::Usage::kOcclusion, &texture.iridescence},
						 std::tuple{&material.iridescenceThicknessTexture, gfx::image::Usage::kMetallicRoughness, &texture.iridescenceThickness}})
					if (!ref->empty())
						loads.push_back({ref->path, ref->embeddedImage, {.usage = usage}, result});
			// the diffuse transmission's factor is the texture's alpha, its color srgb
			if (layers.diffuseTransmission)
				for (auto [ref, usage, result] : std::array{
						 std::tuple{&material.diffuseTransmissionTexture, gfx::image::Usage::kAlpha, &texture.diffuseTransmission},
						 std::tuple{&material.diffuseTransmissionColorTexture, gfx::image::Usage::kColor, &texture.diffuseTransmissionColor}})
					if (!ref->empty())
						loads.push_back({ref->path, ref->embeddedImage, {.usage = usage}, result});
			// the multiple scattering albedo is an srgb color
			if (layers.volumeScatter && !material.multiscatterColorTexture.empty())
				loads.push_back({material.multiscatterColorTexture.path, material.multiscatterColorTexture.embeddedImage, {.usage = gfx::image::Usage::kColor}, &texture.multiscatterColor});
			// the retroreflection's weight is the texture's red
			if (layers.retroreflection && !material.retroreflectionTexture.empty())
				loads.push_back({material.retroreflectionTexture.path, material.retroreflectionTexture.embeddedImage, {.usage = gfx::image::Usage::kOcclusion}, &texture.retroreflection});
			// the transmission's factor is the texture's red, the thickness the green
			if (layers.transmission && !material.transmissionTexture.empty())
				loads.push_back({material.transmissionTexture.path, material.transmissionTexture.embeddedImage, {.usage = gfx::image::Usage::kOcclusion}, &texture.transmission});
			if ((layers.transmission || layers.diffuseTransmission || layers.volumeScatter) && !material.thicknessTexture.empty())
				loads.push_back({material.thicknessTexture.path, material.thicknessTexture.embeddedImage, {.usage = gfx::image::Usage::kMetallicRoughness}, &texture.thickness});
			if (layers.sheen)
				for (auto [ref, usage, result] : std::array{
						 std::tuple{&material.sheenColorTexture, gfx::image::Usage::kColor, &texture.sheenColor},
						 std::tuple{&material.sheenRoughnessTexture, gfx::image::Usage::kAlpha, &texture.sheenRoughness}})
					if (!ref->empty())
						loads.push_back({ref->path, ref->embeddedImage, {.usage = usage}, result});
		}

		// each distinct texture once (its cache entry is keyed the same way, so no two loads write the same files), on the
		// executor's threads (see TaskExecutor::ForEach)
		core::UnorderedMap<std::string, size_t> distinctIndex;
		std::vector<const TextureLoad*> distinct;
		std::vector<size_t> loadDistinct(loads.size());
		for (size_t loadIt = 0; loadIt < loads.size(); loadIt++)
		{
			const auto& load = loads[loadIt];
			auto key = std::format(
				"{}|{}|{}|{}", load.path, load.embeddedImage.value_or(~0U), std::to_underlying(load.options.usage), load.options.bumpScale);
			auto [it, inserted] = distinctIndex.try_emplace(std::move(key), distinct.size());
			if (inserted)
				distinct.push_back(&load);
			loadDistinct[loadIt] = it->second;
		}

		std::vector<Texture> loaded(distinct.size());
		progress = 0;
		std::atomic_size_t done = 0;
		core::Application::Get()->GetExecutor().ForEach(
			distinct.size(),
			[&distinct, &loaded, &progress, &done, cancelled](size_t textureIt)
			{
				if (core::Application::Get()->IsExitRequested() || cancelled())
					return;

				const auto& load = *distinct[textureIt];
				std::atomic_uint8_t textureProgress = 0;
				loaded[textureIt] = LoadTexture(load.path, textureProgress, load.options, load.embeddedImage, cancelled);
				progress = static_cast<uint8_t>(255 * ++done / distinct.size());
			});
		if (core::Application::Get()->IsExitRequested() || cancelled())
			return std::nullopt;

		for (size_t loadIt = 0; loadIt < loads.size(); loadIt++)
			*loads[loadIt].result = loaded[loadDistinct[loadIt]];

		ENSUREF(
			loaded.size() <= kModelTextureMaxCount,
			"{}: {} textures, more than a model may have ({}, SHADER_TYPES_GLOBAL_TEXTURE_COUNT)",
			filePath,
			loaded.size(),
			kModelTextureMaxCount);

		auto [installTask, installFuture] = core::CreateTask<QueueTimelineContextData*>(
			[this, &rhi, model, textures = std::move(textures), filePaths, arrangement](QueueTimelineContextData* graphics) mutable
			{ InstallModel(rhi, *graphics, model, std::move(textures), std::move(filePaths), arrangement); });
		rhi.drawCalls.enqueue(installTask);
		return filePath;
	}

	// loads an image and has the draw thread install it, unless the load was cancelled. call from a load (see gLoads).
	void LoadAndInstallImage(RHI& rhi, std::string_view filePath, std::atomic_uint8_t& progress)
	{
		auto [image, imageView, upload, normalYUp] = LoadTexture(filePath, progress);
		if (!image) // cancelled or failed
			return;

		auto [installTask, installFuture] = core::CreateTask<QueueTimelineContextData*>(
			[this, &rhi, image, imageView, upload](QueueTimelineContextData* graphics)
			{ InstallImage(rhi, *graphics, image, imageView, upload); });
		rhi.drawCalls.enqueue(installTask);
	}


	// makes an uploaded environment the one the scene is lit by, retiring the previous one. call on the draw thread.
	void InstallEnvironment(
		RHI& rhi,
		QueueTimelineContextData& graphics,
		const std::shared_ptr<const EnvironmentTexture>& environment,
		std::string name,
		std::string path)
	{
		ZoneScopedN("WindowedApplication::InstallEnvironment");

		rhi::CrashEvent(std::format("environment installed: {}", name));

		// a file's panoramas are uploaded (the sky has none)
		Uploads uploads;
		for (const auto* texture : {&environment->original, &environment->lighting})
			if (*texture)
				uploads.images.emplace_back(texture->image, texture->upload);
		TransitionThenBind(
			rhi,
			graphics,
			uploads,
			[this, &rhi, environment, name = std::move(name), path = std::move(path)](QueueTimelineContextData& graphics) mutable
			{
				auto& device = rhi.GetPrimaryDevice();
				auto& pipeline = myPipeline;
				const auto& [image, view, upload, normalYUp] = environment->texture;
				const auto& [sheenImage, sheenView, sheenUpload, sheenNormalYUp] = environment->sheenTexture;
				auto& environmentBuffer = *device.GetResource<Buffer>(myEnvironmentUuid);

				EnvironmentData data{
					.textureId = SHADER_TYPES_ENVIRONMENT_TEXTURE,
					.samplerId = kDefaultSamplerId,
					.levelCount = static_cast<float>(environment->levelCount),
					.sheenLevelCount = static_cast<float>(environment->sheenLevelCount)};
				// the sky for the sun and the moon where the time puts them now, through its air (UpdateSky draws it again as they
				// change)
				auto sky = SkyNow();
				auto aerosols = sky.aerosols;
				if (environment->sky)
					FillSkyData(data, sky);
				// the irradiance is the filter's to write, after the rest
				UpdateBufferOnGraphics(graphics, environmentBuffer, 0, std::as_bytes(std::span(&data, 1)));

				// prefiltered on the gpu, before the frames that sample it (the same queue); what the filter reads (the source,
				// a file's panoramas) kept until the gpu is done, and the sky's for as long as it is installed
				RecordEnvironment(graphics, *environment, environmentBuffer, aerosols);
				RetireAfterGraphicsWork(graphics, std::make_shared<std::shared_ptr<const EnvironmentTexture>>(environment));
				mySky = environment->sky ? environment : nullptr;
				RememberSkyDrawn(sky);
				mySkyInstalled.store(environment->sky, std::memory_order_relaxed);

				pipeline.BindLayoutAuto(pipeline.GetLayoutHandle("Main"), PipelineBindPoint::kGraphics);
				pipeline.SetDescriptorData(
					"gTextures",
					ImageBinding{.sampler = {}, .imageView = *view, .layout = image->GetDesc().layout},
					DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
					SHADER_TYPES_ENVIRONMENT_TEXTURE);
				pipeline.SetDescriptorData(
					"gTextures",
					ImageBinding{.sampler = {}, .imageView = *sheenView, .layout = sheenImage->GetDesc().layout},
					DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
					SHADER_TYPES_ENVIRONMENT_SHEEN_TEXTURE);

				RetireAfterGraphicsWork(graphics, device.ReplaceResource(myEnvironmentImageUuid, image));
				RetireAfterGraphicsWork(graphics, device.ReplaceResource(myEnvironmentViewUuid, view));
				RetireAfterGraphicsWork(graphics, device.ReplaceResource(myEnvironmentSheenImageUuid, sheenImage));
				RetireAfterGraphicsWork(graphics, device.ReplaceResource(myEnvironmentSheenViewUuid, sheenView));
				myEnvironmentImageUuid = image->GetUuid();
				myEnvironmentViewUuid = view->GetUuid();
				myEnvironmentSheenImageUuid = sheenImage->GetUuid();
				myEnvironmentSheenViewUuid = sheenView->GetUuid();
				myEnvironmentLights = environment->sky ? SkyLights(sky) : environment->dominantLights;
				myEnvironmentName.Write().Get() = std::move(name);
				myEnvironmentPath.Write().Get() = std::move(path);
			});
	}


	// loads an environment panorama (or without one, the sky) and has the draw thread install it, unless the
	// load was cancelled. call from a load (see gLoads).
	void LoadAndInstallEnvironment(
		RHI& rhi, std::optional<std::string> filePath, std::atomic_uint8_t& progress, core::CancelledFn cancelled = core::NeverCancelled)
	{
		auto environment = std::make_shared<const EnvironmentTexture>(
			gfx::LoadEnvironment(filePath ? std::optional<std::string_view>(*filePath) : std::nullopt, progress, cancelled));
		if (!*environment || cancelled()) // cancelled or failed
			return;

		auto name = filePath ? std::filesystem::path(*filePath).filename().string() : std::string("Physical sky");
		auto [installTask, installFuture] = core::CreateTask<QueueTimelineContextData*>(
			[this, &rhi, environment = std::move(environment), name = std::move(name), path = filePath.value_or(std::string())](
				QueueTimelineContextData* graphics) { InstallEnvironment(rhi, *graphics, environment, name, path); });
		rhi.drawCalls.enqueue(installTask);
	}

	// the size frames are rendered at (see Settings::upscaling): RenderScale of the output's each way, rounded
	[[nodiscard]] rhi::Extent2d RenderExtent() const
	{
		auto extent = myOutput.GetExtent();
		float scale = RenderScale(myUpscaling);
		return {
			.width = std::max(static_cast<uint32_t>(std::lround(static_cast<float>(extent.width) * scale)), 1U),
			.height = std::max(static_cast<uint32_t>(std::lround(static_cast<float>(extent.height) * scale)), 1U)};
	}
	// and over the output's, each way (the views' viewports' scale)
	[[nodiscard]] glm::vec2 RenderViewportScale() const
	{
		auto extent = myOutput.GetExtent();
		auto render = RenderExtent();
		return {
			static_cast<float>(render.width) / static_cast<float>(std::max(extent.width, 1U)),
			static_cast<float>(render.height) / static_cast<float>(std::max(extent.height, 1U))};
	}
	[[nodiscard]] static std::vector<ViewportCreateDesc> ScaledViewports(std::vector<ViewportCreateDesc> viewports, glm::vec2 scale)
	{
		for (auto& viewport : viewports)
			viewport = ScaleViewport(viewport, scale);
		return viewports;
	}

	// (re)creates what is sized after the output (see RenderOutput), which its owner has (re)created. call with the gpu
	// idle
	void CreateOutputDependentObjects(RHI& rhi)
	{
		ZoneScopedN("CreateOutputDependentObjects");
		
		auto& device = rhi.GetPrimaryDevice();
		auto& pipeline = myPipeline;
		auto imageCount = myOutput.GetImageCount();
		ENSUREF(imageCount <= SHADER_TYPES_OUTPUT_IMAGE_COUNT, "{} output images, past SHADER_TYPES_OUTPUT_IMAGE_COUNT ({})", imageCount, SHADER_TYPES_OUTPUT_IMAGE_COUNT);
		auto extent = myOutput.GetExtent();

		// the render target, the transparency's lists and the transmission texture: the renderer's frame graph's, at the
		// render size (a share of the output's with temporal upscaling)
		myUpscaling = mySettings.upscaling.load(std::memory_order_relaxed);
		myRenderer->Resize(device, pipeline, RenderExtent(), extent);
		myTransientMemory = myRenderer->GetGraph().GetTransientMemorySize();
		myUnaliasedTransientMemory = myRenderer->GetGraph().GetUnaliasedTransientMemorySize();

		{
			auto graphics = device.GetQueue(kQueueTypeGraphics).Write();
			auto& [graphicsQueue, graphicsSubmits] = graphics->queues.Get();
			
			auto cmd = graphicsQueue.GetPool().Begin();

			myOutput.Prepare(cmd);

			myRenderer->Prepare(cmd);

			cmd.End();

			graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo{
				.waitSemaphores = {},
				.waitDstStageMasks = {},
				.waitSemaphoreValues = {},
				.signalSemaphores = {graphics->semaphore},
				.signalSemaphoreValues = {++graphics->timeline}},
				std::move(cmd));

			graphicsSubmits |= graphicsQueue.Submit();
		}

		// Draw only writes the current output image's element of these arrays, but a dirty set is updated as a whole:
		// after a resize the other images' elements would still reference the destroyed views. so write all of them up
		// front, with the layouts Draw uses (so Draw's own writes are skipped as unchanged).
		pipeline.BindLayoutAuto(pipeline.GetLayoutHandle("Main"), PipelineBindPoint::kCompute);
		pipeline.SetDescriptorData(
			"gExposureHistogram",
			BufferBinding{.buffer = *device.GetResource<Buffer>(myExposureHistogramUuid), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
		// every output slot: past the image count (fewer images than before) the first image's, never read, rather than
		// the views of a swapchain since destroyed, which the whole set's next update handed the driver (validation's
		// VUID-VkWriteDescriptorSet-descriptorType-02996 after going from 4 images to 3; NVIDIA's driver crashed reading
		// one, or lost the device)
		for (unsigned imageIt = 0; imageIt < SHADER_TYPES_OUTPUT_IMAGE_COUNT; imageIt++)
			pipeline.SetDescriptorData(
				"gRWTextures",
				ImageBinding{
					.sampler={},
					.imageView=myOutput.GetImageView(imageIt < imageCount ? imageIt : 0),
					.layout=ImageLayout::kGeneral},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES,
				imageIt);

		// the output's views and the frame graph's transients were destroyed and made again: a new one with a destroyed one's
		// handle (which the driver may reuse) is skipped above as unchanged, and its set would keep the destroyed one's
		// descriptor (see Pipeline::InvalidateDescriptorSets)
		pipeline.InvalidateDescriptorSets();
	}

	Impl(RHI& rhi, Pipeline& pipeline, Views& views, RenderOutput& output)
		: myRHI(rhi)
		, myPipeline(pipeline)
		, myViews(views)
		, myOutput(output)
	{
		auto& device = rhi.GetPrimaryDevice();

		std::vector<core::TaskHandle> timelineCallbacks;

		constexpr uint32_t kBlackTextureWidth = 4;
		constexpr uint32_t kBlackTextureHeight = 4;
		constexpr uint32_t kBlackTextureSize = kBlackTextureWidth * kBlackTextureHeight * 4;
		
		auto blackTexture = device.CreateResource<Image>(
			ImageCreateDesc{
				device.CreateDeviceObjectCreateDesc("Black Texture"),
				{ImageMipLevelDesc{.extent = Extent2d{.width=kBlackTextureWidth, .height=kBlackTextureHeight}, .size = kBlackTextureSize, .offset = 0}},
				Format::kR8G8B8A8Unorm,
				ImageTiling::kLinear,
				ImageUsage::kSampled | ImageUsage::kTransferDestination,
				MemoryProperty::kDeviceLocal,
				ImageAspect::kColor,
				ImageLayout::kUndefined
			});
		auto blackTextureView = device.CreateResource<ImageView>(
			ImageViewCreateDesc{
				device.CreateDeviceObjectCreateDesc("Black Texture View"),
				*blackTexture,
				blackTexture->GetDesc().format,
				ImageAspect::kColor});
		myBlackTextureUuid = blackTexture->GetUuid();
		myBlackTextureViewUuid = blackTextureView->GetUuid();

		constexpr float kDefaultSamplerMaxAnisotropy = 16.0F;
		// the default, and the clamping one (SHADER_TYPES_CLAMP_SAMPLER)
		std::vector<SamplerDesc> samplerDescs{
			SamplerDesc{.maxAnisotropy = kDefaultSamplerMaxAnisotropy},
			SamplerDesc{
				.addressModeU = AddressMode::kClampToEdge, .addressModeV = AddressMode::kClampToEdge, .addressModeW = AddressMode::kClampToEdge}};
		auto samplers = device.CreateResource<SamplerVector>(
			SamplerVectorCreateDesc{
				device.CreateDeviceObjectCreateDesc("Samplers"),
				std::move(samplerDescs)});
		mySamplersUuid = samplers->GetUuid();

		// initialize stuff on graphics queue
		static_assert(kDefaultSamplerId < SHADER_TYPES_GLOBAL_SAMPLER_COUNT);
		{
			auto graphics = device.GetQueue(kQueueTypeGraphics).Write();
			auto& [graphicsQueue, graphicsSubmits] = graphics->queues.Get();
			
			auto cmd = graphicsQueue.GetPool().Begin();

			blackTexture->Transition(cmd, ImageLayout::kTransferDestination);
			blackTexture->Clear(cmd, {.color = {0.0F, 0.0F, 0.0F, 1.0F}});
			blackTexture->Transition(cmd, ImageLayout::kShaderReadOnly);

			// white and untextured, until InstallModel and InstallImage fill them in
			std::vector<MaterialData> materialData(SHADER_TYPES_MATERIAL_COUNT, DefaultMaterialData());

			core::TaskCreateInfo<void> materialTransfersDone;
			auto materials = device.CreateResource<Buffer>(
				BufferCreateDesc{
					device.CreateDeviceObjectCreateDesc("Materials"),
					SHADER_TYPES_MATERIAL_COUNT * sizeof(MaterialData),
					BufferUsage::kStorage,
					MemoryProperty::kHostVisible},
				materialData.data(),
				cmd,
				materialTransfersDone);
			myMaterialsUuid = materials->GetUuid();
			timelineCallbacks.emplace_back(materialTransfersDone.handle);

			// filled in by InstallModel and InstallImage
			std::vector<TextureView> textureViewData(SHADER_TYPES_TEXTURE_VIEW_COUNT);
			core::TaskCreateInfo<void> textureViewTransfersDone;
			auto textureViews = device.CreateResource<Buffer>(
				BufferCreateDesc{
					device.CreateDeviceObjectCreateDesc("TextureViews"),
					SHADER_TYPES_TEXTURE_VIEW_COUNT * sizeof(TextureView),
					BufferUsage::kStorage,
					MemoryProperty::kHostVisible},
				textureViewData.data(),
				cmd,
				textureViewTransfersDone);
			myTextureViewsUuid = textureViews->GetUuid();
			timelineCallbacks.emplace_back(textureViewTransfersDone.handle);

			// bound until a model is installed, which binds its own instance buffer (see Model::GetInstanceBuffer)
			constexpr uint32_t kMatrix4x4ElementCount = 16;
			std::vector<ModelInstance> modelInstances(1);
			static const auto kIdentityMatrix = glm::mat4x4(1.0);
			std::copy_n(&kIdentityMatrix[0][0], kMatrix4x4ElementCount, &modelInstances[0].modelTransform[0][0]);
			std::copy_n(&kIdentityMatrix[0][0], kMatrix4x4ElementCount, &modelInstances[0].inverseTransposeModelTransform[0][0]);

			core::TaskCreateInfo<void> modelTransfersDone;
			auto modelInstancesBuffer = device.CreateResource<Buffer>(
				BufferCreateDesc{
					device.CreateDeviceObjectCreateDesc("ModelInstances"),
					modelInstances.size() * sizeof(ModelInstance),
					BufferUsage::kStorage,
					MemoryProperty::kHostVisible
				},
				modelInstances.data(),
				cmd,
				modelTransfersDone);
			myModelInstancesUuid = modelInstancesBuffer->GetUuid();
			timelineCallbacks.emplace_back(modelTransfersDone.handle);

			// written by SyncLights
			std::vector<LightData> lightData(SHADER_TYPES_LIGHT_COUNT);
			core::TaskCreateInfo<void> lightTransfersDone;
			auto lights = device.CreateResource<Buffer>(
				BufferCreateDesc{
					device.CreateDeviceObjectCreateDesc("Lights"),
					lightData.size() * sizeof(LightData),
					BufferUsage::kStorage,
					MemoryProperty::kHostVisible},
				lightData.data(),
				cmd,
				lightTransfersDone);
			myLightsUuid = lights->GetUuid();
			timelineCallbacks.emplace_back(lightTransfersDone.handle);

			// dark (and sampling the black texture) until an environment is installed (see InstallEnvironment)
			std::array<EnvironmentData, 1> environmentData{
				EnvironmentData{
					.textureId = SHADER_TYPES_ENVIRONMENT_TEXTURE, .samplerId = kDefaultSamplerId, .levelCount = 1.0F, .sheenLevelCount = 1.0F}};
			core::TaskCreateInfo<void> environmentTransfersDone;
			auto environmentBuffer = device.CreateResource<Buffer>(
				BufferCreateDesc{
					device.CreateDeviceObjectCreateDesc("Environment"),
					sizeof(environmentData),
					BufferUsage::kStorage,
					MemoryProperty::kHostVisible},
				environmentData.data(),
				cmd,
				environmentTransfersDone);
			myEnvironmentUuid = environmentBuffer->GetUuid();
			timelineCallbacks.emplace_back(environmentTransfersDone.handle);

			// counted into by ComputeMain, read by UpdateAutoExposure
			std::vector<uint32_t> histograms(static_cast<size_t>(SHADER_TYPES_FRAME_COUNT) * SHADER_TYPES_FRAME_HISTOGRAM_BINS);
			core::TaskCreateInfo<void> histogramTransfersDone;
			auto histogramBuffer = device.CreateResource<Buffer>(
				BufferCreateDesc{
					device.CreateDeviceObjectCreateDesc("Exposure Histogram"),
					histograms.size() * sizeof(uint32_t),
					BufferUsage::kStorage | BufferUsage::kTransferDestination,
					MemoryProperty::kHostVisible | MemoryProperty::kHostCoherent},
				histograms.data(),
				cmd,
				histogramTransfersDone);
			myExposureHistogramUuid = histogramBuffer->GetUuid();
			timelineCallbacks.emplace_back(histogramTransfersDone.handle);

			std::array<SkinVertex, 1> defaultSkinVertices{};
			core::TaskCreateInfo<void> skinTransfersDone;
			auto skinVertices = device.CreateResource<Buffer>(
				BufferCreateDesc{
					device.CreateDeviceObjectCreateDesc("DefaultSkinVertices"),
					sizeof(defaultSkinVertices),
					BufferUsage::kStorage,
					MemoryProperty::kHostVisible},
				defaultSkinVertices.data(),
				cmd,
				skinTransfersDone);
			myDefaultSkinVerticesUuid = skinVertices->GetUuid();
			timelineCallbacks.emplace_back(skinTransfersDone.handle);

			std::array<glm::mat4, 1> defaultJoints{gfx::mesh::kIdentityTransform};
			core::TaskCreateInfo<void> jointTransfersDone;
			auto joints = device.CreateResource<Buffer>(
				BufferCreateDesc{
					device.CreateDeviceObjectCreateDesc("DefaultJoints"),
					sizeof(defaultJoints),
					BufferUsage::kStorage,
					MemoryProperty::kHostVisible},
				defaultJoints.data(),
				cmd,
				jointTransfersDone);
			myDefaultJointsUuid = joints->GetUuid();
			timelineCallbacks.emplace_back(jointTransfersDone.handle);

			std::array<MorphDelta, 1> defaultMorphDeltas{};
			core::TaskCreateInfo<void> morphDeltaTransfersDone;
			auto morphDeltas = device.CreateResource<Buffer>(
				BufferCreateDesc{
					device.CreateDeviceObjectCreateDesc("DefaultMorphDeltas"),
					sizeof(defaultMorphDeltas),
					BufferUsage::kStorage,
					MemoryProperty::kHostVisible},
				defaultMorphDeltas.data(),
				cmd,
				morphDeltaTransfersDone);
			myDefaultMorphDeltasUuid = morphDeltas->GetUuid();
			timelineCallbacks.emplace_back(morphDeltaTransfersDone.handle);

			std::array<glm::vec4, BVH_NODE_FLOAT4_COUNT> defaultBvh{};
			static_assert(sizeof(EmitterTriangle) <= sizeof(defaultBvh) && sizeof(EmitterNode) <= sizeof(defaultBvh)); // bound as gEmitters and gEmitterTree too
			core::TaskCreateInfo<void> bvhTransfersDone;
			auto bvh = device.CreateResource<Buffer>(
				BufferCreateDesc{
					device.CreateDeviceObjectCreateDesc("DefaultBvh"),
					sizeof(defaultBvh),
					BufferUsage::kStorage,
					MemoryProperty::kHostVisible},
				defaultBvh.data(),
				cmd,
				bvhTransfersDone);
			myDefaultBvhUuid = bvh->GetUuid();
			timelineCallbacks.emplace_back(bvhTransfersDone.handle);

			std::array<float, 1> defaultMorphWeights{};
			core::TaskCreateInfo<void> morphWeightTransfersDone;
			auto morphWeights = device.CreateResource<Buffer>(
				BufferCreateDesc{
					device.CreateDeviceObjectCreateDesc("DefaultMorphWeights"),
					sizeof(defaultMorphWeights),
					BufferUsage::kStorage,
					MemoryProperty::kHostVisible},
				defaultMorphWeights.data(),
				cmd,
				morphWeightTransfersDone);
			myDefaultMorphWeightsUuid = morphWeights->GetUuid();
			timelineCallbacks.emplace_back(morphWeightTransfersDone.handle);

			if (device.GetLimits().rayQuery)
			{
				myDefaultAccelerationStructures = std::make_unique<AccelerationStructures>(device, "Default", AccelerationStructures::Inputs{});
				ENSURE(myDefaultAccelerationStructures->RecordBuild(cmd) == nullptr); // nothing static: no scratch
			}

			cmd.End();

			graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo{
				.waitSemaphores = {},
				.waitDstStageMasks = {},
				.waitSemaphoreValues = {},
				.signalSemaphores = {graphics->semaphore},
				.signalSemaphoreValues = {++graphics->timeline},
				.callbacks = std::move(timelineCallbacks)},
				std::move(cmd));

			graphicsSubmits |= graphicsQueue.Submit();
		}

		auto shaderIncludePath = std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["RootPath"]) / "src/gfx/shaders";
		auto shaderIntermediatePath = std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["UserProfilePath"]) / ".slang.intermediate";

		ShaderLoader shaderLoader({shaderIncludePath}, {}, shaderIntermediatePath);

		auto shaderSourceFile = shaderIncludePath / "shaders.slang";

		// the device's ray queries where it has them (BVH_RAY_QUERY, see AccelerationStructures), else gfx's BVHs traversed
		auto definitions = ShaderLoader::SlangConfiguration{}.preprocessorDefinitions;
		if (device.GetLimits().rayQuery)
			definitions.emplace_back("BVH_RAY_QUERY", "1");
		if (device.GetLimits().rayQuery && RayQueryWatchOn())
			definitions.emplace_back("BVH_RAY_QUERY_WATCH", "1");

		auto zPrepassLayout = pipeline.CreateLayout(
			"VertexZPrepass",
			shaderLoader.Load(
				shaderSourceFile,
				{
					.sourceLanguage = SLANG_SOURCE_LANGUAGE_SLANG,
					.entryPoints = {{"VertexZPrepass", SLANG_STAGE_VERTEX}},
					.optimizationLevel = SLANG_OPTIMIZATION_LEVEL_MAXIMAL,
					.debugInfoLevel = SLANG_DEBUG_INFO_LEVEL_MAXIMAL,
					.preprocessorDefinitions = definitions,
				}));

		pipeline.BindLayoutAuto(zPrepassLayout, PipelineBindPoint::kGraphics);

		for (uint32_t frameIt = 0; frameIt < SHADER_TYPES_FRAME_COUNT; frameIt++)
		{
			pipeline.SetDescriptorData(
				"gModelInstances",
				BufferBinding{.buffer = *device.GetResource<Buffer>(myModelInstancesUuid), .offset = 0},
				DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
				frameIt);
			pipeline.SetDescriptorData(
				"gJointMatrices",
				BufferBinding{.buffer = *device.GetResource<Buffer>(myDefaultJointsUuid), .offset = 0},
				DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
				frameIt);
			pipeline.SetDescriptorData(
				"gMorphWeights",
				BufferBinding{.buffer = *device.GetResource<Buffer>(myDefaultMorphWeightsUuid), .offset = 0},
				DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
				frameIt);
			for (const char* binding : {"gBvhMovingNodes", "gBvhMovingTriangles"})
				pipeline.SetDescriptorData(
					binding,
					BufferBinding{.buffer = *device.GetResource<Buffer>(myDefaultBvhUuid), .offset = 0},
					DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
					frameIt);
		}
		pipeline.SetDescriptorData(
			"gSkinVertices",
			BufferBinding{.buffer = *device.GetResource<Buffer>(myDefaultSkinVerticesUuid), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
		pipeline.SetDescriptorData(
			"gMorphDeltas",
			BufferBinding{.buffer = *device.GetResource<Buffer>(myDefaultMorphDeltasUuid), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
		for (const char* binding : {"gBvhNodes", "gBvhTriangles", "gEmitters", "gEmitterTree"})
			pipeline.SetDescriptorData(
				binding, BufferBinding{.buffer = *device.GetResource<Buffer>(myDefaultBvhUuid), .offset = 0}, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);

		for (uint8_t i = 0; i < SHADER_TYPES_FRAME_COUNT; i++)
		{
			pipeline.SetDescriptorData(
				"gViewData",
				BufferBinding{.buffer = myViews.GetBuffer(i), .offset = 0},
				DESCRIPTOR_SET_CATEGORY_VIEW,
				i);
		}

		ShaderLoader::SlangConfiguration mainConfiguration{
			.sourceLanguage = SLANG_SOURCE_LANGUAGE_SLANG,
			.entryPoints = {
				{"VertexMain", SLANG_STAGE_VERTEX},
				{"FragmentMain", SLANG_STAGE_FRAGMENT},
				{"FragmentTransparent", SLANG_STAGE_FRAGMENT}, // see kTransparentFragmentShader
				{"FragmentShadow", SLANG_STAGE_FRAGMENT}, // see kShadowFragmentShader
				{"FragmentScatter", SLANG_STAGE_FRAGMENT}, // see kScatterFragmentShader
				{"FragmentPrepass", SLANG_STAGE_FRAGMENT}, // see kPrepassFragmentShader
				{"FragmentShadowTransmittance", SLANG_STAGE_FRAGMENT}, // see kShadowTransmittanceFragmentShader
				{"ComputeMain", SLANG_STAGE_COMPUTE},
				{"ComputeScatter", SLANG_STAGE_COMPUTE}, // see the Scatter Blur pass
				{"ComputeScatterDenoise", SLANG_STAGE_COMPUTE}, // see the Scatter Denoise pass
				{"ComputeUiBlurX", SLANG_STAGE_COMPUTE}, // see the UI Blur passes
				{"ComputeUiBlurY", SLANG_STAGE_COMPUTE},
				{"ComputeCull", SLANG_STAGE_COMPUTE}, // see the Cull passes
				{"ComputeDepthPyramid", SLANG_STAGE_COMPUTE}, // see the Depth Pyramid pass
				{"ComputeDepthSave", SLANG_STAGE_COMPUTE}, // see the depth reprojection passes
				{"ComputeDepthReproject", SLANG_STAGE_COMPUTE},
				{"ComputeGiShift", SLANG_STAGE_COMPUTE}, // see the GI passes
				{"ComputeGiVoxelize", SLANG_STAGE_COMPUTE},
				{"ComputeGiLightStatic", SLANG_STAGE_COMPUTE},
				{"ComputeGiLight", SLANG_STAGE_COMPUTE},
				{"ComputeGiFlags", SLANG_STAGE_COMPUTE},
				{"ComputeGiTrace", SLANG_STAGE_COMPUTE},
				{"ComputeGiProbeUpdate", SLANG_STAGE_COMPUTE},
				{"ComputeGiResolve", SLANG_STAGE_COMPUTE},
				{"ComputeGiRestore", SLANG_STAGE_COMPUTE},
				{"ComputeShadowClassify", SLANG_STAGE_COMPUTE}, // see the Shadow Classify, Trace and Denoise passes
				{"ComputeShadowTrace", SLANG_STAGE_COMPUTE},
				{"ComputeShadowDenoiseX", SLANG_STAGE_COMPUTE},
				{"ComputeShadowDenoiseY", SLANG_STAGE_COMPUTE},
				{"ComputeGiCacheFilter", SLANG_STAGE_COMPUTE}, // the GI pass's, after the resolve (last: the indices before stay)
				{"ComputeGiCachePrepare", SLANG_STAGE_COMPUTE}, // and before it: the free pages,
				{"ComputeGiCacheRelease", SLANG_STAGE_COMPUTE}, // and those of the blocks no longer near a surface
				{"ComputeGiNear", SLANG_STAGE_COMPUTE}, // see the GI Near passes
				{"ComputeGiNearX", SLANG_STAGE_COMPUTE},
				{"ComputeGiNearY", SLANG_STAGE_COMPUTE},
				{"ComputeGiNearTemporal", SLANG_STAGE_COMPUTE},
				{"ComputeEmitterSample", SLANG_STAGE_COMPUTE}, // see the Emitter passes
				{"ComputeEmitterShade", SLANG_STAGE_COMPUTE},
				{"ComputeEmitterX", SLANG_STAGE_COMPUTE},
				{"ComputeEmitterY", SLANG_STAGE_COMPUTE},
				{"ComputeEmitterTemporal", SLANG_STAGE_COMPUTE},
				{"ComputeUpscalePrepareInputs", SLANG_STAGE_COMPUTE}, // see the Upscale passes (temporal upscaling)
				{"ComputeUpscaleShadingChangePyramid", SLANG_STAGE_COMPUTE},
				{"ComputeUpscaleShadingChange", SLANG_STAGE_COMPUTE},
				{"ComputeUpscalePrepareReactivity", SLANG_STAGE_COMPUTE},
				{"ComputeUpscaleLumaInstability", SLANG_STAGE_COMPUTE},
				{"ComputeUpscaleAccumulate", SLANG_STAGE_COMPUTE},
				{"ComputeSharpen", SLANG_STAGE_COMPUTE},
				{"ComputeGiResolveTraced", SLANG_STAGE_COMPUTE}, // see the GI Resolve step
				{"ComputeMainVisibility", SLANG_STAGE_COMPUTE}, // see the Main Visibility pass
				{"ComputeMainVisibilityClassify", SLANG_STAGE_COMPUTE}, // see the Main Visibility Classify pass
				{"ComputeReflectionX", SLANG_STAGE_COMPUTE}, // see the Reflection passes (the traced glossy reflections)
				{"ComputeReflectionY", SLANG_STAGE_COMPUTE},
				{"ComputeReflectionTemporal", SLANG_STAGE_COMPUTE},
				{"ComputeTransmissionCopy", SLANG_STAGE_COMPUTE}, // see the Transmission pass
				{"ComputeCaustics", SLANG_STAGE_COMPUTE}, // see the Caustics pass
				{"ComputeCausticsResolve", SLANG_STAGE_COMPUTE},
				{"ComputeReflectionTrace", SLANG_STAGE_COMPUTE}, // see the Reflection Trace pass
				{"ComputeAerialPerspective", SLANG_STAGE_COMPUTE}, // see the Aerial Perspective pass
				{"ComputeSkyEyeView", SLANG_STAGE_COMPUTE}, // see the Sky Eye View pass
				{"ComputeLens", SLANG_STAGE_COMPUTE}, // see the Lens pass
				{"ComputeBloomDown", SLANG_STAGE_COMPUTE}, // see the Bloom pass
				{"ComputeBloomUp", SLANG_STAGE_COMPUTE},
				{"ComputeFlareSources", SLANG_STAGE_COMPUTE}, // see the Flare Sources pass
				{"ComputeDofTiles", SLANG_STAGE_COMPUTE}, // see the DOF passes
				{"ComputeDofTilesDilate", SLANG_STAGE_COMPUTE},
				{"ComputeDofPrepare", SLANG_STAGE_COMPUTE},
				{"ComputeDofGather", SLANG_STAGE_COMPUTE},
				{"ComputeDofComposite", SLANG_STAGE_COMPUTE},
				{"ComputeMotionVelocity", SLANG_STAGE_COMPUTE}, // see the Motion passes
				{"ComputeMotionTiles", SLANG_STAGE_COMPUTE},
				{"ComputeMotionTilesDilate", SLANG_STAGE_COMPUTE},
				{"ComputeMotionBlur", SLANG_STAGE_COMPUTE},
				{"ComputeFlareTrack", SLANG_STAGE_COMPUTE}, // see the Flare Track pass
			},
			.optimizationLevel = SLANG_OPTIMIZATION_LEVEL_MAXIMAL,
			.debugInfoLevel = SLANG_DEBUG_INFO_LEVEL_MAXIMAL,
			.preprocessorDefinitions = definitions,
		};
		// diagnostic: the ray query replay's kernel (see gfx/rayquerywatch.h), after the others (the renderer's
		// kRayQueryReplayEntryPoint), where the shaders have the watch
		if (device.GetLimits().rayQuery && RayQueryWatchOn())
			mainConfiguration.entryPoints.emplace_back("ComputeRayQueryReplay", SLANG_STAGE_COMPUTE);
		auto mainLayout = pipeline.CreateLayout("Main", shaderLoader.Load(shaderSourceFile, mainConfiguration));

		// the glare's flares' layout (see flare.slang, the renderer's Flare pass): a vertex and a fragment stage of its own
		[[maybe_unused]] auto flareLayout = pipeline.CreateLayout(
			"Flare",
			shaderLoader.Load(
				shaderIncludePath / "flare.slang",
				{
					.sourceLanguage = SLANG_SOURCE_LANGUAGE_SLANG,
					.entryPoints = {{"VertexFlare", SLANG_STAGE_VERTEX}, {"FragmentFlare", SLANG_STAGE_FRAGMENT}},
					.optimizationLevel = SLANG_OPTIMIZATION_LEVEL_MAXIMAL,
					.debugInfoLevel = SLANG_DEBUG_INFO_LEVEL_MAXIMAL,
				}));

		pipeline.BindLayoutAuto(mainLayout, PipelineBindPoint::kGraphics);

		myEnvironmentFilter = std::make_unique<EnvironmentFilter>(device, pipeline, shaderLoader, shaderIncludePath / "environment.slang");
		pipeline.BindLayoutAuto(pipeline.GetLayoutHandle("Main"), PipelineBindPoint::kGraphics);
		// the sky's view of itself, which the backdrop samples where the sky is the environment (see SkyRadiance), and its
		// other tables, which the aerial perspective is made from (see ComputeAerialPerspective)
		for (auto [view, slot] : std::array{
				 std::pair{&myEnvironmentFilter->GetSkyView(), SHADER_TYPES_SKY_VIEW_TEXTURE},
				 std::pair{&myEnvironmentFilter->GetSkyTransmittance(), SHADER_TYPES_SKY_TRANSMITTANCE_TEXTURE},
				 std::pair{&myEnvironmentFilter->GetSkyMultipleScattering(), SHADER_TYPES_SKY_MULTIPLE_SCATTERING_TEXTURE},
				 std::pair{&myEnvironmentFilter->GetSkyMoonView(), SHADER_TYPES_SKY_MOON_VIEW_TEXTURE}})
			pipeline.SetDescriptorData(
				"gTextures",
				ImageBinding{.sampler = {}, .imageView = *view, .layout = ImageLayout::kGeneral},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
				slot);

		pipeline.SetDescriptorData(
			"gMaterialData",
			BufferBinding{.buffer = *device.GetResource<Buffer>(myMaterialsUuid), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_MATERIAL);

		pipeline.SetDescriptorData(
			"gLights",
			BufferBinding{.buffer = *device.GetResource<Buffer>(myLightsUuid), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);
		if (myDefaultAccelerationStructures)
			BindAccelerationStructures(*myDefaultAccelerationStructures);
		if (device.GetLimits().rayQuery && RayQueryWatchOn())
		{
			myRayQueryWatch = std::make_unique<RayQueryWatch>(device);
			pipeline.SetDescriptorData(
				"gRayQueryWatch", BufferBinding{.buffer = myRayQueryWatch->GetBuffer(), .offset = 0}, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);
			pipeline.SetDescriptorData(
				"gRayQueryReplay", BufferBinding{.buffer = myRayQueryWatch->GetReplayBuffer(), .offset = 0}, DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);
		}

		pipeline.SetDescriptorData(
			"gEnvironment",
			BufferBinding{.buffer = *device.GetResource<Buffer>(myEnvironmentUuid), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);
		pipeline.SetDescriptorData(
			"gTextures",
			ImageBinding{.sampler = {}, .imageView = *device.GetResource<ImageView>(myBlackTextureViewUuid), .layout = ImageLayout::kShaderReadOnly},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
			SHADER_TYPES_ENVIRONMENT_TEXTURE);
		pipeline.SetDescriptorData(
			"gTextures",
			ImageBinding{.sampler = {}, .imageView = *device.GetResource<ImageView>(myBlackTextureViewUuid), .layout = ImageLayout::kShaderReadOnly},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_TEXTURES,
			SHADER_TYPES_ENVIRONMENT_SHEEN_TEXTURE);

		pipeline.SetDescriptorData(
			"gTextureViews",
			BufferBinding{.buffer = *device.GetResource<Buffer>(myTextureViewsUuid), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_MATERIAL);

		for (uint32_t frameIt = 0; frameIt < SHADER_TYPES_FRAME_COUNT; frameIt++)
		{
			pipeline.SetDescriptorData(
				"gModelInstances",
				BufferBinding{.buffer = *device.GetResource<Buffer>(myModelInstancesUuid), .offset = 0},
				DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
				frameIt);
			pipeline.SetDescriptorData(
				"gJointMatrices",
				BufferBinding{.buffer = *device.GetResource<Buffer>(myDefaultJointsUuid), .offset = 0},
				DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
				frameIt);
			pipeline.SetDescriptorData(
				"gMorphWeights",
				BufferBinding{.buffer = *device.GetResource<Buffer>(myDefaultMorphWeightsUuid), .offset = 0},
				DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
				frameIt);
			for (const char* binding : {"gBvhMovingNodes", "gBvhMovingTriangles"})
				pipeline.SetDescriptorData(
					binding,
					BufferBinding{.buffer = *device.GetResource<Buffer>(myDefaultBvhUuid), .offset = 0},
					DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES,
					frameIt);
		}
		pipeline.SetDescriptorData(
			"gSkinVertices",
			BufferBinding{.buffer = *device.GetResource<Buffer>(myDefaultSkinVerticesUuid), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
		pipeline.SetDescriptorData(
			"gMorphDeltas",
			BufferBinding{.buffer = *device.GetResource<Buffer>(myDefaultMorphDeltasUuid), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
		for (const char* binding : {"gBvhNodes", "gBvhTriangles", "gEmitters", "gEmitterTree"})
			pipeline.SetDescriptorData(
				binding, BufferBinding{.buffer = *device.GetResource<Buffer>(myDefaultBvhUuid), .offset = 0}, DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);

		pipeline.SetDescriptorData(
			"gSamplers",
			ImageBinding{.sampler = (*device.GetResource<SamplerVector>(mySamplersUuid))[0]},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS,
			kDefaultSamplerId);
		pipeline.SetDescriptorData(
			"gSamplers",
			ImageBinding{.sampler = (*device.GetResource<SamplerVector>(mySamplersUuid))[1]},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_SAMPLERS,
			SHADER_TYPES_CLAMP_SAMPLER);

		for (uint8_t i = 0; i < SHADER_TYPES_FRAME_COUNT; i++)
		{
			pipeline.SetDescriptorData(
				"gViewData",
				BufferBinding{.buffer = myViews.GetBuffer(i), .offset = 0},
				DESCRIPTOR_SET_CATEGORY_VIEW,
				i);
		}

		pipeline.BindLayoutAuto(pipeline.GetLayoutHandle("Main"), PipelineBindPoint::kCompute);

		myRenderer = std::make_unique<Renderer>(device, myOutput);
		CreateOutputDependentObjects(rhi);
	}

	void Update(QueueTimelineContextData& graphics, uint32_t frameIndex)
	{
		myMovingPlaced = false;
		// what doesn't move compacted, once its build is done (see AccelerationStructures::RecordCompaction)
		if (auto* structures = myModel ? myModel->GetAccelerationStructures() : nullptr; structures != nullptr && structures->CanCompact())
		{
			std::shared_ptr<void> previous;
			RecordOnGraphics(graphics, [&previous, structures](CommandBufferHandle cmd) { previous = structures->RecordCompaction(cmd); });
			RetireAfterGraphicsWork(graphics, std::move(previous));
		}
		// the frame's exposure histogram, and instance and joint buffers, now that the frame's previous use of them is done
		// (see the fences above)
		UpdateAutoExposure(myRHI, frameIndex);
		if (myModel && myModel->Moves())
		{
			ScenePose pose;
			ScenePose from;
			float weight = 1.0F;
			{
				auto animation = myAnimation.Write();
				auto& state = animation.Get();
				auto now = std::chrono::steady_clock::now();
				auto elapsed = std::chrono::duration<double>(now - state.last).count();
				state.last = now;
				// for comparing runs: as many seconds a drawn frame (SPEEDO_ANIMATION_FRAME_SECONDS), so that two runs have
				// what moves where it was at the same frame, whatever their loads and frames took (as the time of day's
				// SPEEDO_TIME_FRAME_SECONDS)
				static const double kFrameSeconds = []
				{
					const char* seconds = std::getenv("SPEEDO_ANIMATION_FRAME_SECONDS");
					return seconds != nullptr && *seconds != '\0' ? std::max(std::strtod(seconds, nullptr), 0.0) : -1.0;
				}();
				if (kFrameSeconds >= 0.0)
					elapsed = kFrameSeconds;
				if (state.playing)
				{
					state.time += elapsed;
					state.previousTime += elapsed;
				}
				// the fade runs in real time, also while paused
				state.fade = std::min(state.fade + elapsed, AnimationState::kCrossfade);
				pose = {.animation = state.selected, .time = static_cast<float>(state.time)};
				from = {.animation = state.previous, .time = static_cast<float>(state.previousTime)};
				weight = static_cast<float>(state.fade / AnimationState::kCrossfade);
			}
			// the moving BVH only where rays trace it: the indirect lighting's, or the traced shadows', the emitters', or (with the
			// device's ray queries, whose structures hold the glass that moves too) the traced reflections', refractions' and the
			// caustics' photons'
			bool rayQueries = myModel->GetAccelerationStructures() != nullptr;
			myMovingPlaced =
				(mySettings.indirectLighting.load(std::memory_order_relaxed) && mySettings.traceTriangles.load(std::memory_order_relaxed)) ||
				(mySettings.shadows.load(std::memory_order_relaxed) && mySettings.tracedShadows.load(std::memory_order_relaxed)) ||
				(mySettings.emissiveLights.load(std::memory_order_relaxed) && myModel->GetEmitterCount() > 0) ||
				(rayQueries &&
				 (mySettings.tracedReflections.load(std::memory_order_relaxed) || mySettings.tracedRefractions.load(std::memory_order_relaxed) ||
				  (mySettings.shadows.load(std::memory_order_relaxed) && mySettings.caustics.load(std::memory_order_relaxed))));
			myModel->Animate(frameIndex, pose, from, weight, myMovingPlaced);
			ApplyPointerValues(myRHI, graphics, *myModel);
			// the pose decides all that moves (the nodes, skins and morph weights, and the animated materials, lights and
			// visibility): the same as the last frame's (paused, SPEEDO_ANIMATION_TIME), nothing moved
			PosedAs posed{
				.model = myModel.get(),
				.animation = pose.animation,
				.time = pose.time,
				.previous = from.animation,
				.previousTime = from.time,
				.weight = weight};
			myModelStill = myPosedAs == posed;
			myPosedAs = posed;
		}
		else
			myModelStill = true;
		
		UpdateSky(myRHI, graphics);
		SyncLights(myRHI, graphics);
	}

	void Record(CommandBufferHandle cmd, const FrameTarget& target)
	{
		auto& device = myRHI.GetPrimaryDevice();
		auto& pipeline = myPipeline;

		// what moves, where this frame's Update placed it, for the device's ray queries
		if (myModel && myMovingPlaced && myModel->GetAccelerationStructures() != nullptr)
			myModel->GetAccelerationStructures()->RecordUpdate(cmd, target.frameIndex);

		// which materials are transmissive (the main pass's second phase), counting those an animation turns on, and which
		// layer groups their pipelines can skip (unless SPEEDO_SHADER_TIERS=0: the generic pipelines for all)
		static const bool kTiers = [] { const char* tiers = std::getenv("SPEEDO_SHADER_TIERS"); return tiers == nullptr || std::string_view(tiers) != "0"; }();
		std::vector<bool> transmissive;
		std::vector<bool> scatters;
		std::vector<uint16_t> specialization;
		if (myModel)
			for (size_t materialIt = 0; materialIt < myModel->GetDesc().materials.size(); materialIt++)
			{
				auto layers = LayersOf(myModel->GetDesc(), materialIt);
				transmissive.push_back(layers.transmission);
				// drawn into the scatter buffer too (unless the pipelines skip the extended layers, see Layer)
				scatters.push_back(layers.volumeScatter);
				bool extended = layers.clearcoat || layers.sheen || layers.transmission || layers.anisotropy || layers.iridescence ||
								layers.diffuseTransmission || layers.retroreflection || layers.volumeScatter;
				specialization.push_back(kTiers && !extended ? static_cast<uint16_t>(SHADER_TYPES_LAYERS_EXTENDED) : uint16_t{0});
			}
		auto specializationOf = [&specialization](size_t material)
		{ return material < specialization.size() ? specialization[material] : uint16_t{0}; };

		// the views as this frame draws them (the cameras may move while it records): what it culls by and fits its
		// shadows (to each view) and clusters to
		auto& views = myViews;
		auto viewportScale = RenderViewportScale();
		std::array<float, SHADER_TYPES_SPLIT_VIEW_COUNT> exposures{};
		std::array<float, SHADER_TYPES_SPLIT_VIEW_COUNT> scotopics{};
		std::array<float, SHADER_TYPES_SPLIT_VIEW_COUNT> lookDistances{};
		for (size_t viewIt = 0; viewIt < exposures.size(); viewIt++)
		{
			exposures[viewIt] = ViewExposure(viewIt);
			scotopics[viewIt] = ViewScotopic(viewIt);
			lookDistances[viewIt] = myLookDistances[viewIt].load(std::memory_order_relaxed);
		}
		// the camera's lens, which sets the field the views render (see FrameLens), before they upload
		auto lensData = FrameLens(views);
		auto cameras = views.Upload(target.frameIndex, viewportScale, myUpscaling != Upscaling::kOff, exposures, scotopics);
		std::vector<glm::mat4> viewProjections;
		for (const auto& camera : cameras)
			viewProjections.push_back(camera.GetProjectionMatrix() * camera.GetViewMatrix());
		std::vector<shadows::ShadowCamera> shadowCameras;
		for (size_t cameraIt = 0; cameraIt < cameras.size(); cameraIt++)
			shadowCameras.push_back(
				{.view = cameras[cameraIt].GetViewMatrix(),
				 .inverseViewProjection = glm::inverse(viewProjections[cameraIt]),
				 .nearPlane = cameras[cameraIt].GetDesc().nearPlane,
				 .farPlane = cameras[cameraIt].GetDesc().farPlane});
		auto skySun = SkySunWorld();
		auto airLight = SkyAirLight();
		myRenderer->Record(
			cmd,
			pipeline,
			FrameInputs{
				.frameIndex = target.frameIndex,
				.outputIndex = target.outputIndex,
				.pushConstants = FramePushConstants(target.frameIndex, target.outputIndex),
				.model = myModel.get(),
				.modelStill = myModelStill,
				.transmissive = [&transmissive](size_t material) { return material < transmissive.size() && transmissive[material]; },
				.scatters = [&scatters](size_t material) { return material < scatters.size() && scatters[material]; },
				.specialization = kTiers ? std::optional<core::std_extra::function_ref<uint16_t(size_t)>>(specializationOf) : std::nullopt,
				.grid = views.GetGrid(),
				.viewports = ScaledViewports(views.GetViewports(), viewportScale),
				.viewProjections = std::move(viewProjections),
				.culling = mySettings.culling.load(std::memory_order_relaxed),
				.clusteredLights = mySettings.clusteredLights.load(std::memory_order_relaxed),
				.indirectLighting = mySettings.indirectLighting.load(std::memory_order_relaxed),
				.traceTriangles = mySettings.traceTriangles.load(std::memory_order_relaxed),
				.giResolution = mySettings.giResolution.load(std::memory_order_relaxed),
				.giSurfaceCache = mySettings.giSurfaceCache.load(std::memory_order_relaxed),
				.giNearField = mySettings.giNearField.load(std::memory_order_relaxed),
				.giNearContact = mySettings.giNearContact.load(std::memory_order_relaxed),
				.giNearStride = mySettings.giNearStride.load(std::memory_order_relaxed),
				.giNearTemporal = mySettings.giNearTemporal.load(std::memory_order_relaxed),
				.lights = myLightData,
				.modelLightCount = std::min(mySceneLightData.size(), myLightData.size()),
				.captureLights = mySettings.captureLights.load(std::memory_order_relaxed),
				.shadows = mySettings.shadows.load(std::memory_order_relaxed),
				.tracedShadows = mySettings.tracedShadows.load(std::memory_order_relaxed),
				.tracedShadowsEverywhere = mySettings.tracedShadowsEverywhere.load(std::memory_order_relaxed),
				.tracedReflections = mySettings.tracedReflections.load(std::memory_order_relaxed),
				.tracedRefractions = mySettings.tracedRefractions.load(std::memory_order_relaxed),
				.caustics = mySettings.caustics.load(std::memory_order_relaxed),
				.emissiveLights = mySettings.emissiveLights.load(std::memory_order_relaxed),
				.aerialPerspective = mySettings.aerialPerspective.load(std::memory_order_relaxed) && mySkyInstalled.load(std::memory_order_relaxed),
				.skySun = skySun,
				.airLight = airLight,
				.metersPerUnit = mySettings.metersPerUnit.load(std::memory_order_relaxed),
				.shaftLight = mySettings.lightShafts.load(std::memory_order_relaxed) ? SkySunLight(glm::vec3(airLight)) : std::nullopt,
				.fogVisibility = mySettings.fog.load(std::memory_order_relaxed) ? mySettings.fogVisibility.load(std::memory_order_relaxed) : 0.0F,
				.fogHeight = mySettings.fogHeight.load(std::memory_order_relaxed),
				.sky = mySkyInstalled.load(std::memory_order_relaxed),
				.skyAerosols = mySkyAerosolsDrawn,
				.cameras = std::move(shadowCameras),
				.sceneBounds = myModel ? myModel->GetDesc().bounds : Bounds3f{},
				.casterBounds = myModel ? myModel->GetFramedBounds() : Bounds3f{},
				.lookDistances = lookDistances,
				.exposureHistogram = &*device.GetResource<Buffer>(myExposureHistogramUuid),
				.graphicsQueue = target.graphicsQueue,
				.graphicsTimeline = target.graphicsTimeline,
				.prepareUi = target.prepareUi,
				.drawUi = target.drawUi,
				.uiBlurRects = target.uiBlurRects,
				.upscaling = myUpscaling,
				.lens = lensData,
				.lensGhosts = myLensState.ghosts,
				.executor = target.executor});
	}
};

Scene::Scene(RHI& rhi, Pipeline& pipeline, Views& views, RenderOutput& output)
	: myImpl(std::make_unique<Impl>(rhi, pipeline, views, output))
{}

Scene::~Scene() = default;

std::vector<DescriptorPoolSize> Scene::DescriptorPoolSizes()
{
	return gfx::DescriptorPoolSizes();
}

std::optional<std::string> Scene::LoadModels(
	const std::vector<std::string>& filePaths,
	std::atomic_uint8_t& progress,
	const mesh::ImportOptions& options,
	ModelArrangement arrangement,
	ModelGround ground,
	core::CancelledFn cancelled)
{
	return myImpl->LoadAndInstallModels(myImpl->myRHI, filePaths, progress, options, arrangement, ground, cancelled);
}

std::optional<std::string> Scene::LoadVariants(
	std::span<const std::string> filePaths,
	std::span<const std::optional<uint32_t>> variants,
	std::optional<size_t> scene,
	std::atomic_uint8_t& progress,
	ModelArrangement arrangement,
	ModelGround ground,
	core::CancelledFn cancelled)
{
	ENSURE(!filePaths.empty() && filePaths.size() == variants.size());

	auto optionsOf = [scene](std::optional<uint32_t> variant)
	{ return mesh::ImportOptions{.scene = scene, .variant = variant ? std::optional<size_t>(*variant) : std::nullopt}; };
	if (filePaths.size() == 1)
		return myImpl->LoadAndInstallModels(myImpl->myRHI, {filePaths.front()}, progress, optionsOf(variants.front()), arrangement, ground, cancelled);
	std::vector<mesh::ImportOptions> fileOptions;
	for (auto variant : variants)
		fileOptions.push_back(optionsOf(variant));
	return myImpl->LoadAndInstallModels(
		myImpl->myRHI, std::vector(filePaths.begin(), filePaths.end()), progress, {}, arrangement, ground, cancelled, fileOptions);
}

void Scene::LoadImage(std::string_view filePath, std::atomic_uint8_t& progress)
{
	myImpl->LoadAndInstallImage(myImpl->myRHI, filePath, progress);
}

void Scene::LoadEnvironment(std::optional<std::string> filePath, std::atomic_uint8_t& progress, core::CancelledFn cancelled)
{
	myImpl->LoadAndInstallEnvironment(myImpl->myRHI, std::move(filePath), progress, cancelled);
}

void Scene::Resize()
{
	myImpl->CreateOutputDependentObjects(myImpl->myRHI);
}

bool Scene::NeedsResize() const noexcept
{
	return myImpl->mySettings.upscaling.load(std::memory_order_relaxed) != myImpl->myUpscaling;
}

void Scene::Update(QueueTimelineContextData& graphics, uint32_t frameIndex)
{
	myImpl->Update(graphics, frameIndex);
}

void Scene::Record(CommandBufferHandle cmd, const FrameTarget& target)
{
	myImpl->Record(cmd, target);
}

Scene::Settings& Scene::GetSettings() noexcept { return myImpl->mySettings; }
core::ConcurrentAccess<Scene::AnimationState>& Scene::GetAnimation() noexcept { return myImpl->myAnimation; }
core::ConcurrentAccess<Scene::SceneState>& Scene::GetScenes() noexcept { return myImpl->myScenes; }
bool Scene::IsSkyInstalled() const noexcept { return myImpl->mySkyInstalled.load(std::memory_order_relaxed); }
bool Scene::HasRayQueries() const noexcept { return myImpl->myRHI.GetPrimaryDevice().GetLimits().rayQuery; }
core::ConcurrentAccess<std::string>& Scene::GetEnvironmentName() noexcept { return myImpl->myEnvironmentName; }
core::ConcurrentAccess<std::string>& Scene::GetEnvironmentPath() noexcept { return myImpl->myEnvironmentPath; }
core::ConcurrentAccess<std::vector<Scene::DominantLightInfo>>& Scene::GetEnvironmentLightInfo() noexcept { return myImpl->myEnvironmentLightInfo; }
core::ConcurrentAccess<std::vector<float>>& Scene::GetLightSizes() noexcept { return myImpl->myLightSizes; }
camera::Exposure Scene::GetViewExposure() const noexcept { return myImpl->ChosenExposure(0); }

camera::ExposureLimits Scene::GetExposureLimits() const noexcept { return myImpl->LensLimits(); }

float Scene::GetFocusDistance() const noexcept { return myImpl->FocusDistanceMeters(); }
uint64_t Scene::GetTransientMemory() const noexcept { return myImpl->myTransientMemory.load(std::memory_order_relaxed); }
uint64_t Scene::GetUnaliasedTransientMemory() const noexcept { return myImpl->myUnaliasedTransientMemory.load(std::memory_order_relaxed); }
const Renderer& Scene::GetRenderer() const noexcept { return *myImpl->myRenderer; }

} // namespace gfx
