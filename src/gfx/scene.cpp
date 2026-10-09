#include "scene.h"

#include <core/application.h>
#include <core/profiling.h>
#include <core/task.h>
#include <gfx/environment.h>
#include <gfx/environmentfilter.h>
#include <gfx/gpu.h>
#include <gfx/graphicsqueue.h>
#include <gfx/meshimport.h>
#include <gfx/model.h>
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
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
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
};


[[nodiscard]] static MaterialLayers LayersOf(const ModelDesc& desc, size_t materialIndex)
{
	const auto& material = desc.materials[materialIndex];
	MaterialLayers layers{
		.clearcoat = material.clearcoat > 0.0F,
		.sheen = std::ranges::any_of(material.sheenColor, [](float c) { return c > 0.0F; }),
		.transmission = material.transmission > 0.0F,
		.anisotropy = material.anisotropy > 0.0F,
		.iridescence = material.iridescence > 0.0F,
		.diffuseTransmission = material.diffuseTransmission > 0.0F};
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
		{.type = rhi::DescriptorType::kSampledImage, .count = kGlobalResourceBaseCount * SHADER_TYPES_GLOBAL_TEXTURE_COUNT},
		{.type = rhi::DescriptorType::kStorageImage, .count = kGlobalResourceBaseCount * SHADER_TYPES_GLOBAL_RW_TEXTURE_COUNT},
		{.type = rhi::DescriptorType::kUniformTexelBuffer, .count = kBufferBaseCount},
		{.type = rhi::DescriptorType::kStorageTexelBuffer, .count = kBufferBaseCount},
		{.type = rhi::DescriptorType::kUniformBuffer, .count = kBufferBaseCount},
		{.type = rhi::DescriptorType::kStorageBuffer, .count = kBufferBaseCount},
		{.type = rhi::DescriptorType::kUniformBufferDynamic, .count = kBufferBaseCount},
		{.type = rhi::DescriptorType::kStorageBufferDynamic, .count = kBufferBaseCount},
		{.type = rhi::DescriptorType::kInputAttachment, .count = kBufferBaseCount},
	};
}


// gTextures slots 0 to SHADER_TYPES_FRAME_COUNT - 1 hold the frames' render targets (for ComputeMain), and
// SHADER_TYPES_ENVIRONMENT_TEXTURE and SHADER_TYPES_ENVIRONMENT_SHEEN_TEXTURE the environment's (and
// SHADER_TYPES_TRANSMISSION_TEXTURE the opaque scene). material 0 is the default material, for models (or parts of them) without one: it samples this slot, which opening an image replaces.
static constexpr uint32_t kMaterialTextureId = 15;

// the loaded model's materials are 1 and up, and their textures are in the slots from here up
static constexpr uint32_t kModelTextureFirstSlot = 16;

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
	Views& myViews;
	Settings mySettings;

	// draws the frames (see Renderer): its frame graph owns what is sized by the swapchain
	std::unique_ptr<Renderer> myRenderer;
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

	uuids::uuid mySamplersUuid;
	uuids::uuid myModelSamplersUuid; // the loaded model's samplers, see InstallModel. nil until one is loaded
	uuids::uuid myMaterialsUuid;
	uuids::uuid myTextureViewsUuid;
	uuids::uuid myModelInstancesUuid;
	// bound as gSkinVertices, gJointMatrices, gMorphDeltas and gMorphWeights while the installed model has none (a model
	// that doesn't move, isn't skinned or has no animated morph targets): one zero skin vertex, one identity joint matrix,
	// one zero morph delta and one zero weight
	uuids::uuid myDefaultSkinVerticesUuid;
	uuids::uuid myDefaultJointsUuid;
	uuids::uuid myDefaultMorphDeltasUuid;
	uuids::uuid myDefaultMorphWeightsUuid;
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
	std::atomic<float> myAutoExposureStops = 0.0F; // written by the draw thread, shown by the ui
	std::optional<std::chrono::steady_clock::time_point> myAutoExposureLast;
	uuids::uuid myExposureHistogramUuid; // gExposureHistogram: SHADER_TYPES_EXPOSURE_BINS per frame, host visible
	size_t myModelSamplerCount = 0; // how many of kModelSamplerSlots the loaded model uses


	// the loaded model's textures, by slot (from kModelTextureFirstSlot). nil for slots it doesn't use.
	std::vector<std::pair<uuids::uuid, uuids::uuid>> myModelTextureUuids; // image, view
	std::vector<MaterialData> myModelMaterialData;
	std::vector<AnimatedTextureView> myAnimatedTextureViews;
	std::vector<SceneLight> myAnimatedLights; // the lights as last uploaded, if some follow nodes


	// the push constants every draw (and ComputeMain) of a frame shares
	PushConstants FramePushConstants(uint16_t frameIndex)
	{
		return PushConstants{
			.frameIndex = frameIndex,
			.lightCount = myLightCount,
			.exposure = std::exp2(
				mySettings.exposureStops.load(std::memory_order_relaxed) +
				(mySettings.autoExposure.load(std::memory_order_relaxed) ? myAutoExposureStops.load(std::memory_order_relaxed) : 0.0F)),
			.environmentIntensity = mySettings.environmentIntensity.load(std::memory_order_relaxed),
			.environmentRotation = glm::radians(mySettings.environmentRotationDegrees.load(std::memory_order_relaxed)),
			.viewCount = std::min<uint32_t>(myViews.GetGrid().x * myViews.GetGrid().y, SHADER_TYPES_VIEW_COUNT),
			.environmentBackdrop = mySettings.environmentBackdrop.load(std::memory_order_relaxed) ? 1U : 0U,
			.tonemapper = mySettings.tonemapper.load(std::memory_order_relaxed),
			.shadowView = SHADER_TYPES_NO_SHADOW};
	}


	// writes materials, starting at slot first (see UpdateBufferOnGraphics). call on the draw thread.
	void UpdateMaterials(
		RHI& rhi, QueueTimelineContextData& graphics, uint32_t first, std::span<const MaterialData> materials)
	{
		ENSURE(first + materials.size() <= SHADER_TYPES_MATERIAL_COUNT);

		UpdateBufferOnGraphics(
			graphics, *rhi.GetPrimaryDevice().GetResource<Buffer>(myMaterialsUuid), first * sizeof(MaterialData), std::as_bytes(materials));
	}


	// writes a model's lights to gLights, or the default light if it has none: a directional light from above that, with
	// the ambient light in the shader, lights matte surfaces as before there were lights. call on the draw thread.
	// sets the installed model's lights (none for a model without), for SyncLights to upload. call on the draw thread.
	void UpdateLights(std::span<const SceneLight> sceneLights)
	{
		// the rest are the environment's dominant lights
		constexpr size_t kModelLightCount = SHADER_TYPES_LIGHT_COUNT - environment::kMaxDominantLights;
		if (sceneLights.size() > kModelLightCount)
		{
			std::println(stderr, "{} lights, only the first {} are used", sceneLights.size(), kModelLightCount);
			sceneLights = sceneLights.first(kModelLightCount);
		}

		std::vector<LightData> lights(sceneLights.size());
		for (size_t lightIt = 0; lightIt < lights.size(); lightIt++)
		{
			const auto& sceneLight = sceneLights[lightIt];
			auto& light = lights[lightIt];
			std::ranges::copy(sceneLight.position, light.positionRange);
			light.positionRange[3] = sceneLight.range;
			std::ranges::copy(sceneLight.direction, light.direction);
			for (size_t channel = 0; channel < 3; channel++)
				light.intensity[channel] = sceneLight.color[channel] * sceneLight.intensity;
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


	// uploads gLights if it changed: the model's lights, then the environment's dominant lights as directional lights
	// (turned and scaled as the environment is: they were taken out of it, see environment::ExtractDominantLights), or
	// before any environment is installed, a light where the procedural sky's sun is. if one of the model's directional
	// lights already shines from where one of them does (within kMergeAngle), they are the same light, merged: the model's
	// (as authored, and maybe animated) is kept alone, unless merging is off (mySettings.mergeLights). call on the draw thread.
	void SyncLights(RHI& rhi, QueueTimelineContextData& graphics)
	{
		std::vector<LightData> lights = mySceneLightData;
		std::vector<environment::DominantLight> dominants(1, environment::DominantLight{.direction = environment::kSkySunDirection});
		dominants.front().irradiance.fill(environment::kSkySunIrradiance);
		if (myEnvironmentLights)
			dominants = *myEnvironmentLights;

		// the panorama's space to the world's (the inverse of the shader's EnvironmentDirection)
		float angle = glm::radians(mySettings.environmentRotationDegrees.load(std::memory_order_relaxed));
		float intensity = mySettings.environmentIntensity.load(std::memory_order_relaxed);
		float c = std::cos(angle);
		float s = std::sin(angle);
		constexpr float kMergeAngle = 5.0F; // degrees
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

			glm::vec3 direction(light.direction[0], light.direction[1], light.direction[2]);
			bool merged = merge && std::ranges::any_of(
									   mySceneLightData,
									   [&direction, cosMerge](const LightData& sceneLight)
									   {
										   glm::vec3 sceneDirection(sceneLight.direction[0], sceneLight.direction[1], sceneLight.direction[2]);
										   return sceneLight.type == LIGHT_TYPE_DIRECTIONAL && glm::length(sceneDirection) > 0.0F &&
												  glm::dot(glm::normalize(sceneDirection), glm::normalize(direction)) >= cosMerge;
									   });
			if (!merged && lights.size() < SHADER_TYPES_LIGHT_COUNT)
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


	// auto exposure: what a frame's histogram (gExposureHistogram, read once the frame's previous use is done) says the
	// scene's luminance is, the mean log2 luminance of the pixels between the 50th and 95th percentile (the darkest and the
	// brightest left out), toward which the exposure moves, a little each frame (by elapsed time), so that it is
	// kAutoExposureKey: the default scene (the procedural sky) keeps the exposure it had before. call on the draw thread.
	void UpdateAutoExposure(RHI& rhi, uint32_t frameIndex)
	{
		constexpr double kAutoExposureKey = 0.3;
		constexpr double kAdaptationSpeed = 2.0; // per second, of what is left to the target, as an exponential decay

		auto now = std::chrono::steady_clock::now();
		auto elapsed = myAutoExposureLast ? std::chrono::duration<double>(now - *myAutoExposureLast).count() : 0.0;
		myAutoExposureLast = now;

		auto& histogram = *rhi.GetPrimaryDevice().GetResource<Buffer>(myExposureHistogramUuid);
		auto memory = histogram.Map();
		std::array<uint32_t, SHADER_TYPES_EXPOSURE_BINS> bins{};
		std::memcpy(bins.data(), memory.data() + (static_cast<size_t>(frameIndex) * sizeof(bins)), sizeof(bins));
		histogram.Unmap();

		uint64_t total = std::accumulate(bins.begin(), bins.end(), uint64_t{0});
		if (total == 0 || !mySettings.autoExposure.load(std::memory_order_relaxed))
			return;
		auto low = static_cast<double>(total) * 0.5;
		auto high = static_cast<double>(total) * 0.95;
		double counted = 0.0;
		double sum = 0.0;
		double weight = 0.0;
		for (size_t binIt = 0; binIt < bins.size(); binIt++)
		{
			// the part of the bin between the percentiles
			auto from = std::max(counted, low);
			auto to = std::min(counted + bins[binIt], high);
			counted += bins[binIt];
			if (to <= from)
				continue;
			auto log2Luminance = SHADER_TYPES_EXPOSURE_LOG2_MIN +
								 ((static_cast<double>(binIt) + 0.5) / SHADER_TYPES_EXPOSURE_BINS * (SHADER_TYPES_EXPOSURE_LOG2_MAX - SHADER_TYPES_EXPOSURE_LOG2_MIN));
			sum += log2Luminance * (to - from);
			weight += to - from;
		}
		if (weight <= 0.0)
			return;
		auto target = static_cast<float>(std::log2(kAutoExposureKey) - (sum / weight));
		// the first measure is taken as it is
		auto stops = myAutoExposureStops.load(std::memory_order_relaxed);
		if (elapsed <= 0.0 || elapsed > 1.0)
			stops = target;
		else
			stops += (target - stops) * static_cast<float>(1.0 - std::exp(-elapsed * kAdaptationSpeed));
		myAutoExposureStops.store(stops, std::memory_order_relaxed);
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


	// makes an uploaded model the one being drawn, with its materials and their textures (by material), retiring the
	// previous ones. call on the draw thread.
	void InstallModel(
		RHI& rhi,
		QueueTimelineContextData& graphics,
		const std::shared_ptr<Model>& model,
		std::vector<MaterialTextures> textures)
	{
		ZoneScopedN("WindowedApplication::InstallModel");

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
				  &material.diffuseTransmission, &material.diffuseTransmissionColor})
				if (texture->image &&
					std::ranges::none_of(uploads.images, [&texture](const auto& image) { return image.first == texture->image; }))
					uploads.images.emplace_back(texture->image, texture->upload);

		// everything is switched at once, after the textures are readable: one descriptor set update, and no frame draws
		// the new model with the old materials or the other way around
		TransitionThenBind(rhi, graphics, uploads, [this, &rhi, model, textures = std::move(textures)](QueueTimelineContextData& graphics)
		{
			auto& device = rhi.GetPrimaryDevice();
			auto& pipeline = device.GetPipeline();
			const auto& blackView = *device.GetResource<ImageView>(myBlackTextureViewUuid);

			pipeline.BindLayoutAuto(device.GetPipelineLayoutHandle("Main"), PipelineBindPoint::kGraphics);

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

				if (textureUuids.size() == kModelTextureMaxCount)
					return std::nullopt;

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
			auto samplerSlotOf = [&samplerDescs, warned = false](const SamplerDesc& desc) mutable -> uint32_t
			{
				static const SamplerDesc kDefault = TextureRef{}.sampler;
				if (desc == kDefault)
					return kDefaultSamplerId;
				auto samplerDescIt = std::ranges::find(samplerDescs, desc);
				if (samplerDescIt == samplerDescs.end())
				{
					if (samplerDescs.size() == kModelSamplerSlots.size())
					{
						if (!std::exchange(warned, true))
							std::println(stderr, "more than {} different samplers, the rest use the default", kModelSamplerSlots.size());
						return kDefaultSamplerId;
					}
					samplerDescIt = samplerDescs.insert(samplerDescs.end(), desc);
				}
				return kModelSamplerSlots[static_cast<size_t>(samplerDescIt - samplerDescs.begin())];
			};

			// each distinct view once, from 1 (0 is material 0's, see InstallImage)
			std::vector<TextureView> views;
			std::vector<AnimatedTextureView> animatedViews;
			core::UnorderedMap<TextureViewKey, uint32_t, TextureViewKeyHash> viewIds;
			bool viewsFull = false;
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
					if (1 + views.size() == SHADER_TYPES_TEXTURE_VIEW_COUNT)
					{
						if (!std::exchange(viewsFull, true))
							std::println(stderr, "more than {} texture views, the rest are left out", SHADER_TYPES_TEXTURE_VIEW_COUNT - 1);
						viewIds.erase(viewIdIt);
						return std::nullopt;
					}
					viewIdIt->second = static_cast<uint32_t>(1 + views.size());
					views.push_back(MakeTextureView(*textureSlot, samplerSlot, ref, texture.normalYUp ? TEXTURE_VIEW_FLAG_NORMAL_Y_UP : 0U));
					if (ref.animatedTransform >= 0)
						animatedViews.push_back({.view = viewIdIt->second, .target = static_cast<uint32_t>(ref.animatedTransform), .data = views.back()});
				}
				return viewIdIt->second;
			};

			std::vector<MaterialData> materials(std::min<size_t>(textures.size(), kModelMaterialMaxCount), DefaultMaterialData());
			for (size_t materialIt = 0; materialIt < materials.size(); materialIt++)
			{
				auto& material = materials[materialIt];
				const auto& desc = descMaterials[materialIt];
				std::ranges::fill(material.color, 1.0F);
				material.alphaCutoff = desc.alphaCutoff;
				std::ranges::copy(desc.emissive, material.emissive);
				material.metallic = desc.metallic;
				material.roughness = desc.roughness;
				material.specular = desc.specular;
				std::ranges::copy(desc.specularColor, material.specularColor);
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
					std::ranges::copy(desc.sheenColor, material.sheen);
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
					material.transmission[1] = desc.thickness;
					material.transmission[2] = desc.attenuationDistance;
					material.transmission[3] = desc.dispersion;
					std::ranges::copy(desc.attenuationColor, material.attenuationColor);
					for (auto [texture, ref, view, flag] : std::array{
							 std::tuple{&textures[materialIt].transmission, &desc.transmissionTexture, &material.transmissionView, MATERIAL_FLAG_TRANSMISSION_TEXTURE},
							 std::tuple{&textures[materialIt].thickness, &desc.thicknessTexture, &material.thicknessView, MATERIAL_FLAG_THICKNESS_TEXTURE}})
						if (auto id = viewOf(*texture, *ref))
						{
							*view = *id;
							material.flags |= flag;
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
					std::ranges::copy(desc.diffuseTransmissionColor, material.diffuseTransmission);
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

			UpdateMaterials(rhi, graphics, 1, materials);
			myModelMaterialData = materials;
			myAnimatedLights.clear();
			UpdateLights(model->GetDesc().lights);
			// and the values animations set, at rest
			ApplyPointerValues(rhi, graphics, *model);

			pipeline.SetDescriptorData(
				"gVertexBuffer",
				BufferBinding{.buffer = model->GetVertexBuffer(), .offset = 0},
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
			}

			{
				auto scenes = myScenes.Write();
				scenes.Get() = SceneState{
					.filePath = model->GetDesc().name, .names = model->GetDesc().scenes, .current = model->GetDesc().scene};
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

			myViews.SetScene(model->GetDesc().bounds, model->GetDesc().cameras);
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

		TransitionThenBind(rhi, graphics, Uploads{.images = {{image, upload}}}, [this, &rhi, image, imageView](QueueTimelineContextData& graphics)
		{
			auto& device = rhi.GetPrimaryDevice();
			auto& pipeline = device.GetPipeline();

			pipeline.BindLayoutAuto(device.GetPipelineLayoutHandle("Main"), PipelineBindPoint::kGraphics);
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
			UpdateMaterials(rhi, graphics, 0, std::span(&material, 1));

			RetireAfterGraphicsWork(graphics, device.ReplaceResource(myLoadedImageUuid, image));
			RetireAfterGraphicsWork(graphics, device.ReplaceResource(myLoadedImageViewUuid, imageView));
			myLoadedImageUuid = image->GetUuid();
			myLoadedImageViewUuid = imageView->GetUuid();
		});
	}


	// loads a model (or several, side by side as one, see Model::Load) and its materials' textures, and has the draw thread
	// install them, unless the load was cancelled. call from a load (see gLoads).
	// scene: a gltf file's scene to load (one file only), else its default one
	void LoadAndInstallModels(
		RHI& rhi, const std::vector<std::string>& filePaths, std::atomic_uint8_t& progress, std::optional<size_t> scene = std::nullopt)
	{
		auto model = scene && filePaths.size() == 1
						 ? Model::Load(filePaths.front(), progress, scene)
						 : Model::Load(std::vector<std::string_view>(filePaths.begin(), filePaths.end()), progress);
		if (!model) // cancelled or failed
			return;

		const auto& filePath = model->GetDesc().name; // or the models' directory, for several

		const auto& materials = model->GetDesc().materials;
		if (materials.size() > kModelMaterialMaxCount)
			std::println(stderr, "{}: {} materials, only the first {} are used", filePath, materials.size(), kModelMaterialMaxCount);

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
			if (!material.emissiveTexture.empty() && std::ranges::any_of(material.emissive, [](float value) { return value > 0.0F; }))
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
			// the transmission's factor is the texture's red, the thickness the green
			if (layers.transmission)
				for (auto [ref, usage, result] : std::array{
						 std::tuple{&material.transmissionTexture, gfx::image::Usage::kOcclusion, &texture.transmission},
						 std::tuple{&material.thicknessTexture, gfx::image::Usage::kMetallicRoughness, &texture.thickness}})
					if (!ref->empty())
						loads.push_back({ref->path, ref->embeddedImage, {.usage = usage}, result});
			if (layers.sheen)
				for (auto [ref, usage, result] : std::array{
						 std::tuple{&material.sheenColorTexture, gfx::image::Usage::kColor, &texture.sheenColor},
						 std::tuple{&material.sheenRoughnessTexture, gfx::image::Usage::kAlpha, &texture.sheenRoughness}})
					if (!ref->empty())
						loads.push_back({ref->path, ref->embeddedImage, {.usage = usage}, result});
		}

		core::UnorderedMap<std::string, Texture> loaded;
		progress = 0;
		for (size_t loadIt = 0; loadIt < loads.size(); loadIt++)
		{
			const auto& load = loads[loadIt];
			auto key = std::format(
				"{}|{}|{}|{}", load.path, load.embeddedImage.value_or(~0U), std::to_underlying(load.options.usage), load.options.bumpScale);
			auto [it, inserted] = loaded.try_emplace(std::move(key));
			if (inserted)
			{
				if (core::Application::Get()->IsExitRequested())
					return;

				std::atomic_uint8_t textureProgress = 0;
				it->second = LoadTexture(load.path, textureProgress, load.options, load.embeddedImage);
			}
			*load.result = it->second;

			progress = static_cast<uint8_t>(255 * (loadIt + 1) / loads.size());
		}

		if (loaded.size() > kModelTextureMaxCount)
			std::println(stderr, "{}: {} textures, only the first {} are used", filePath, loaded.size(), kModelTextureMaxCount);

		auto [installTask, installFuture] = core::CreateTask<QueueTimelineContextData*>(
			[this, &rhi, model, textures = std::move(textures)](QueueTimelineContextData* graphics) mutable
			{ InstallModel(rhi, *graphics, model, std::move(textures)); });
		rhi.drawCalls.enqueue(installTask);
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
		RHI& rhi, QueueTimelineContextData& graphics, const std::shared_ptr<const EnvironmentTexture>& environment, std::string name)
	{
		ZoneScopedN("WindowedApplication::InstallEnvironment");

		// a file's panoramas are uploaded (the procedural sky has none)
		Uploads uploads;
		for (const auto* texture : {&environment->original, &environment->lighting})
			if (*texture)
				uploads.images.emplace_back(texture->image, texture->upload);
		TransitionThenBind(
			rhi,
			graphics,
			uploads,
			[this, &rhi, environment, name = std::move(name)](QueueTimelineContextData& graphics) mutable
			{
				auto& device = rhi.GetPrimaryDevice();
				auto& pipeline = device.GetPipeline();
				const auto& [image, view, upload, normalYUp] = environment->texture;
				const auto& [sheenImage, sheenView, sheenUpload, sheenNormalYUp] = environment->sheenTexture;
				auto& environmentBuffer = *device.GetResource<Buffer>(myEnvironmentUuid);

				EnvironmentData data{
					.textureId = SHADER_TYPES_ENVIRONMENT_TEXTURE,
					.samplerId = kDefaultSamplerId,
					.levelCount = static_cast<float>(environment->levelCount),
					.sheenLevelCount = static_cast<float>(environment->sheenLevelCount)};
				if (const auto& sky = environment->sky)
				{
					std::ranges::copy(sky->zenith, data.skyZenith);
					data.skyZenith[3] = 1.0F;
					std::ranges::copy(sky->horizon, data.skyHorizon);
					std::ranges::copy(sky->ground, data.skyGround);
					std::ranges::copy(sky->sunDirection, data.skySun);
					data.skySun[3] = sky->sunCosRadius;
					data.skySunRadiance[0] = sky->sunRadiance;
				}
				// the irradiance is the filter's to write, after the rest
				UpdateBufferOnGraphics(graphics, environmentBuffer, 0, std::as_bytes(std::span(&data, 1)));

				// prefiltered on the gpu, before the frames that sample it (the same queue)
				{
					auto& [graphicsQueue, graphicsSubmits] = graphics.queues.Get();
					auto cmd = graphicsQueue.GetPool().Commands();
					auto used = myEnvironmentFilter->Record(cmd, pipeline, *environment, environmentBuffer);
					cmd.End();
					graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo{
						.waitSemaphores = {graphics.semaphore},
						.waitDstStageMasks = {PipelineStage::kAllCommands},
						.waitSemaphoreValues = {graphics.timeline},
						.signalSemaphores = {graphics.semaphore},
						.signalSemaphoreValues = {++graphics.timeline},});
					graphicsSubmits |= graphicsQueue.Submit();
					// what the filter reads (the source, a file's panoramas) and its views, until the gpu is done
					RetireAfterGraphicsWork(graphics, std::move(used));
					RetireAfterGraphicsWork(graphics, std::make_shared<std::shared_ptr<const EnvironmentTexture>>(environment));
				}

				pipeline.BindLayoutAuto(device.GetPipelineLayoutHandle("Main"), PipelineBindPoint::kGraphics);
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
				myEnvironmentLights = environment->dominantLights;
				myEnvironmentName.Write().Get() = std::move(name);
			});
	}


	// loads an environment panorama (or without one, the procedural sky) and has the draw thread install it, unless the
	// load was cancelled. call from a load (see gLoads).
	void LoadAndInstallEnvironment(RHI& rhi, std::optional<std::string> filePath, std::atomic_uint8_t& progress)
	{
		auto environment = std::make_shared<const EnvironmentTexture>(
			gfx::LoadEnvironment(filePath ? std::optional<std::string_view>(*filePath) : std::nullopt, progress));
		if (!*environment) // cancelled or failed
			return;

		auto name = filePath ? std::filesystem::path(*filePath).filename().string() : std::string("Procedural sky");
		auto [installTask, installFuture] = core::CreateTask<QueueTimelineContextData*>(
			[this, &rhi, environment = std::move(environment), name = std::move(name)](QueueTimelineContextData* graphics)
			{ InstallEnvironment(rhi, *graphics, environment, name); });
		rhi.drawCalls.enqueue(installTask);
	}

	void CreateWindowDependentObjects(RHI& rhi)
	{
		ZoneScopedN("CreateWindowDependentObjects");
		
		auto& device = rhi.GetPrimaryDevice();
		auto& swapchain = rhi.GetSwapchain(GetCurrentWindow());
		auto& pipeline = device.GetPipeline();
		auto frameCount = swapchain.GetFrames().size();
		ENSURE(frameCount <= SHADER_TYPES_FRAME_COUNT);

		// the render target, the transparency's lists and the transmission texture: the renderer's frame graph's
		myRenderer->Resize(device, pipeline, swapchain.GetDesc().extent);
		myTransientMemory = myRenderer->GetGraph().GetTransientMemorySize();
		myUnaliasedTransientMemory = myRenderer->GetGraph().GetUnaliasedTransientMemorySize();

		{
			auto graphics = device.GetQueue(kQueueTypeGraphics).Write();
			auto& [graphicsQueue, graphicsSubmits] = graphics->queues.Get();
			
			auto cmd = graphicsQueue.GetPool().Commands();

			// no layout transitions for the swapchain images here: they may only be used once acquired. Draw transitions
			// each acquired image from its tracked layout (UNDEFINED for a new swapchain).
			for (auto& frame : swapchain.GetFrames())
			{
				frame.SetLoadOp(LoadOp::kClear, 0);
				frame.SetStoreOp(StoreOp::kStore, 0);
			}

			myRenderer->Prepare(cmd);

			cmd.End();

			graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo{
				.waitSemaphores = {},
				.waitDstStageMasks = {},
				.waitSemaphoreValues = {},
				.signalSemaphores = {graphics->semaphore},
				.signalSemaphoreValues = {++graphics->timeline}});

			graphicsSubmits |= graphicsQueue.Submit();
		}

		// Draw only writes the current frame's element of these arrays, but a dirty set is updated as a whole: after a
		// resize the other frames' elements would still reference the destroyed views. so write all of them up front,
		// with the layouts Draw uses (so Draw's own writes are skipped as unchanged).
		pipeline.BindLayoutAuto(device.GetPipelineLayoutHandle("Main"), PipelineBindPoint::kCompute);
		pipeline.SetDescriptorData(
			"gExposureHistogram",
			BufferBinding{.buffer = *device.GetResource<Buffer>(myExposureHistogramUuid), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
		for (unsigned frameIt = 0; frameIt < frameCount; frameIt++)
			pipeline.SetDescriptorData(
				"gRWTextures",
				ImageBinding{
					.sampler={},
					.imageView=swapchain.GetFrames()[frameIt].GetAttachments()[0],
					.layout=ImageLayout::kGeneral},
				DESCRIPTOR_SET_CATEGORY_GLOBAL_RW_TEXTURES,
				frameIt);
	}

	Impl(RHI& rhi, Views& views)
		: myRHI(rhi)
		, myViews(views)
	{
		auto& device = rhi.GetPrimaryDevice();
		auto& pipeline = device.GetPipeline();

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
			
			auto cmd = graphicsQueue.GetPool().Commands();

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
			std::vector<uint32_t> histograms(static_cast<size_t>(SHADER_TYPES_FRAME_COUNT) * SHADER_TYPES_EXPOSURE_BINS);
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

			std::array<std::array<float, 16>, 1> defaultJoints{gfx::mesh::kIdentityTransform};
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

			cmd.End();

			graphicsQueue.EnqueueSubmit(QueueDeviceSyncInfo{
				.waitSemaphores = {},
				.waitDstStageMasks = {},
				.waitSemaphoreValues = {},
				.signalSemaphores = {graphics->semaphore},
				.signalSemaphoreValues = {++graphics->timeline},
				.callbacks = std::move(timelineCallbacks)});

			graphicsSubmits |= graphicsQueue.Submit();
		}

		auto shaderIncludePath = std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["RootPath"]) / "src/gfx/shaders";
		auto shaderIntermediatePath = std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["UserProfilePath"]) / ".slang.intermediate";

		ShaderLoader shaderLoader({shaderIncludePath}, {}, shaderIntermediatePath);

		auto shaderSourceFile = shaderIncludePath / "shaders.slang";

		const auto& [zPrepassShaderLayoutPairIt, zPrepassShaderLayoutWasInserted] = device.GetPipelineLayoutHandles().emplace(
			std::hash<std::string_view>{}("VertexZPrepass"),
			pipeline.CreateLayout(shaderLoader.Load(
				shaderSourceFile,
				{
					.sourceLanguage = SLANG_SOURCE_LANGUAGE_SLANG,
					.entryPoints = {{"VertexZPrepass", SLANG_STAGE_VERTEX}},
					.optimizationLevel = SLANG_OPTIMIZATION_LEVEL_MAXIMAL,
					.debugInfoLevel = SLANG_DEBUG_INFO_LEVEL_MAXIMAL,
				})));

		pipeline.BindLayoutAuto(zPrepassShaderLayoutPairIt->second, PipelineBindPoint::kGraphics);

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
		}
		pipeline.SetDescriptorData(
			"gSkinVertices",
			BufferBinding{.buffer = *device.GetResource<Buffer>(myDefaultSkinVerticesUuid), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
		pipeline.SetDescriptorData(
			"gMorphDeltas",
			BufferBinding{.buffer = *device.GetResource<Buffer>(myDefaultMorphDeltasUuid), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);

		for (uint8_t i = 0; i < SHADER_TYPES_FRAME_COUNT; i++)
		{
			pipeline.SetDescriptorData(
				"gViewData",
				BufferBinding{.buffer = myViews.GetBuffer(i), .offset = 0},
				DESCRIPTOR_SET_CATEGORY_VIEW,
				i);
		}

		const auto& [mainShaderLayoutPairIt, mainShaderLayoutWasInserted] = device.GetPipelineLayoutHandles().emplace(
			std::hash<std::string_view>{}("Main"),
			pipeline.CreateLayout(shaderLoader.Load(
				shaderSourceFile,
				{
					.sourceLanguage = SLANG_SOURCE_LANGUAGE_SLANG,
					.entryPoints = {
						{"VertexMain", SLANG_STAGE_VERTEX},
						{"FragmentMain", SLANG_STAGE_FRAGMENT},
						{"FragmentTransparent", SLANG_STAGE_FRAGMENT}, // see kTransparentFragmentShader
						{"FragmentShadow", SLANG_STAGE_FRAGMENT}, // see kShadowFragmentShader
						{"ComputeMain", SLANG_STAGE_COMPUTE},
					},
					.optimizationLevel = SLANG_OPTIMIZATION_LEVEL_MAXIMAL,
					.debugInfoLevel = SLANG_DEBUG_INFO_LEVEL_MAXIMAL,
				})));

		pipeline.BindLayoutAuto(mainShaderLayoutPairIt->second, PipelineBindPoint::kGraphics);

		// (registering its layout can rehash the layout map: mainShaderLayoutPairIt is stale after it)
		myEnvironmentFilter = std::make_unique<EnvironmentFilter>(device, pipeline, shaderLoader, shaderIncludePath / "environment.slang");
		pipeline.BindLayoutAuto(device.GetPipelineLayoutHandle("Main"), PipelineBindPoint::kGraphics);

		pipeline.SetDescriptorData(
			"gMaterialData",
			BufferBinding{.buffer = *device.GetResource<Buffer>(myMaterialsUuid), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_MATERIAL);

		pipeline.SetDescriptorData(
			"gLights",
			BufferBinding{.buffer = *device.GetResource<Buffer>(myLightsUuid), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_MODEL_INSTANCES);

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
		}
		pipeline.SetDescriptorData(
			"gSkinVertices",
			BufferBinding{.buffer = *device.GetResource<Buffer>(myDefaultSkinVerticesUuid), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);
		pipeline.SetDescriptorData(
			"gMorphDeltas",
			BufferBinding{.buffer = *device.GetResource<Buffer>(myDefaultMorphDeltasUuid), .offset = 0},
			DESCRIPTOR_SET_CATEGORY_GLOBAL_BUFFERS);

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

		pipeline.BindLayoutAuto(device.GetPipelineLayoutHandle("Main"), PipelineBindPoint::kCompute);

		myRenderer = std::make_unique<Renderer>(device);
		CreateWindowDependentObjects(rhi);
	}

	void Update(QueueTimelineContextData& graphics, uint32_t frameIndex)
	{
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
			myModel->Animate(frameIndex, pose, from, weight);
			ApplyPointerValues(myRHI, graphics, *myModel);
		}
		
		SyncLights(myRHI, graphics);
	}

	void Record(CommandBufferHandle cmd, const FrameTarget& target)
	{
		auto& device = myRHI.GetPrimaryDevice();
		auto& pipeline = device.GetPipeline();

		// which materials are transmissive (the main pass's second phase), counting those an animation turns on, and which
		// layer groups their pipelines can skip (unless SPEEDO_SHADER_TIERS=0: the generic pipelines for all)
		static const bool kTiers = [] { const char* tiers = std::getenv("SPEEDO_SHADER_TIERS"); return tiers == nullptr || std::string_view(tiers) != "0"; }();
		std::vector<bool> transmissive;
		std::vector<uint16_t> specialization;
		if (myModel)
			for (size_t materialIt = 0; materialIt < myModel->GetDesc().materials.size(); materialIt++)
			{
				auto layers = LayersOf(myModel->GetDesc(), materialIt);
				transmissive.push_back(layers.transmission);
				bool extended = layers.clearcoat || layers.sheen || layers.transmission || layers.anisotropy || layers.iridescence ||
								layers.diffuseTransmission;
				specialization.push_back(kTiers && !extended ? static_cast<uint16_t>(SHADER_TYPES_LAYERS_EXTENDED) : uint16_t{0});
			}

		const auto& views = myViews;
		// the shadows are fitted to the first view's camera
		shadows::ShadowCamera shadowCamera;
		if (auto camera = views.GetCamera(0))
		{
			shadowCamera.view = camera->GetViewMatrix();
			shadowCamera.inverseViewProjection = glm::inverse(camera->GetProjectionMatrix() * camera->GetViewMatrix());
			shadowCamera.nearPlane = camera->GetDesc().nearPlane;
			shadowCamera.farPlane = camera->GetDesc().farPlane;
		}
		myRenderer->Record(
			cmd,
			pipeline,
			FrameInputs{
				.frameIndex = target.frameIndex,
				.pushConstants = FramePushConstants(target.frameIndex),
				.model = myModel.get(),
				.transmissive = [&transmissive](size_t material) { return material < transmissive.size() && transmissive[material]; },
				.specialization = kTiers ? std::function<uint16_t(size_t)>([&specialization](size_t material)
										 { return material < specialization.size() ? specialization[material] : uint16_t{0}; })
										 : std::function<uint16_t(size_t)>{},
				.grid = views.GetGrid(),
				.viewports = views.GetViewports(),
				.lights = myLightData,
				.shadows = mySettings.shadows.load(std::memory_order_relaxed),
				.camera = shadowCamera,
				.sceneBounds = myModel ? myModel->GetDesc().bounds : Bounds3f{},
				.swapchain = target.swapchain,
				.swapchainFrame = target.swapchainFrame,
				.exposureHistogram = &*device.GetResource<Buffer>(myExposureHistogramUuid),
				.graphicsQueue = target.graphicsQueue,
				.graphicsTimeline = target.graphicsTimeline,
				.prepareUi = target.prepareUi,
				.drawUi = target.drawUi,
				.runInBackground = target.runInBackground});
	}
};

Scene::Scene(RHI& rhi, Views& views) : myImpl(std::make_unique<Impl>(rhi, views)) {}

Scene::~Scene() = default;

std::vector<DescriptorPoolSize> Scene::DescriptorPoolSizes()
{
	return gfx::DescriptorPoolSizes();
}

void Scene::LoadModels(const std::vector<std::string>& filePaths, std::atomic_uint8_t& progress, std::optional<size_t> scene)
{
	myImpl->LoadAndInstallModels(myImpl->myRHI, filePaths, progress, scene);
}

void Scene::LoadImage(std::string_view filePath, std::atomic_uint8_t& progress)
{
	myImpl->LoadAndInstallImage(myImpl->myRHI, filePath, progress);
}

void Scene::LoadEnvironment(std::optional<std::string> filePath, std::atomic_uint8_t& progress)
{
	myImpl->LoadAndInstallEnvironment(myImpl->myRHI, std::move(filePath), progress);
}

void Scene::Resize()
{
	myImpl->CreateWindowDependentObjects(myImpl->myRHI);
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
core::ConcurrentAccess<std::string>& Scene::GetEnvironmentName() noexcept { return myImpl->myEnvironmentName; }
core::ConcurrentAccess<std::vector<Scene::DominantLightInfo>>& Scene::GetEnvironmentLightInfo() noexcept { return myImpl->myEnvironmentLightInfo; }
float Scene::GetAutoExposureStops() const noexcept { return myImpl->myAutoExposureStops.load(std::memory_order_relaxed); }
uint64_t Scene::GetTransientMemory() const noexcept { return myImpl->myTransientMemory.load(std::memory_order_relaxed); }
uint64_t Scene::GetUnaliasedTransientMemory() const noexcept { return myImpl->myUnaliasedTransientMemory.load(std::memory_order_relaxed); }
const Renderer& Scene::GetRenderer() const noexcept { return *myImpl->myRenderer; }

} // namespace gfx
